/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "uniqueid.h"

#include <map>
#include <new>
#include <string_view>
#include <vector>
#include <utility>
#include <memory>
#include <unordered_map>

class Identifier;

/*
 * Non-local control flow (return/break/continue) is signaled through this
 * struct instead of C++ exceptions. Throwing is ~1.6us on this toolchain
 * (heap-allocated exception object + DWARF table-driven stack unwinding,
 * neither reducible by build flags), which dominated recursion-heavy code
 * because `return` was an exception. Statements set the FlowState, and
 * Block / loops / do_func_call check it and unwind via ordinary C++ returns.
 * Genuinely exceptional paths (runtime errors, user `throw`, `rethrow`) still
 * use C++ exceptions, where the zero-cost-when-not-thrown model is the right
 * fit.
 */
struct FlowState {

    enum Type : unsigned char {
        none,   /* normal execution                                  */
        brk,    /* a `break` in flight, up to the nearest loop       */
        cont,   /* a `continue` in flight, up to the nearest loop    */
        ret,    /* a `return` in flight, up to the function boundary */
    };

    Type type = none;
    EvalValue value;    /* the return value, meaningful when type == ret */
};

/*
 * Per-call storage for resolved local variables (function params and locals).
 *
 * The name-resolution pass assigns each resolved local a fixed slot index, so
 * access is an O(1) array index here instead of a scope-chain map lookup. A
 * call's Frame is created in do_func_call and shared by every nested block of
 * that call (EvalContext::frame is inherited from the parent). There is no
 * per-slot liveness bitmask: every slot is default-constructed when the Frame
 * is built, and a local can only RESOLVE to its slot after its declaration
 * (no-hoist forward resolution), so a slot is always bound before any read -
 * a use-before-decl resolves to an outer binding or errors via the map, never
 * to the slot. (That removed the old 64-slot-per-frame cap; the slot count is
 * unbounded now.)
 *
 * The Frame lives on do_func_call's C++ stack. For the common case
 * (frame_size <= INLINE_SLOTS) its slots are placement-constructed into an
 * inline raw buffer, so a resolved call allocates nothing on the heap; only an
 * unusually large frame spills to the heap vector. Crucially init() constructs
 * EXACTLY frame_size slots (not INLINE_SLOTS), so a 1-slot frame pays for one
 * slot, not eight - the unused inline capacity is just stack bytes. `slots`
 * always points at whichever storage is active, so callers just index slots[i].
 */
/*
 * Per-frame cache of PURE function-call results (the v3 recursion optimization).
 * Key: the callee's FuncDeclStmt + the call's argument values. When a recursive
 * pure function is unrolled into a frame (the InlinedCallExpr shares the caller
 * frame), its duplicate self-calls land here and compute ONCE. Lazy (only calls
 * actually made are stored) so it never evaluates a call the program wouldn't -
 * sound; frame-scoped (dies with the frame) so it is NOT global memoization.
 */
struct PureCacheKey {
    const void *fn;
    std::vector<EvalValue> args;

    bool operator==(const PureCacheKey &o) const {
        return fn == o.fn && args == o.args;
    }
};

struct PureCacheKeyHash {
    size_t operator()(const PureCacheKey &k) const {
        size_t h = reinterpret_cast<size_t>(k.fn);
        for (const auto &a : k.args)
            h = h * 1000003u + a.hash();
        return h;
    }
};

typedef std::unordered_map<PureCacheKey, EvalValue, PureCacheKeyHash> PureCache;

struct Frame {
    static constexpr int INLINE_SLOTS = 8;

    alignas(LValue) char inline_buf[INLINE_SLOTS * sizeof(LValue)];
    std::vector<LValue> heap_buf;   /* spill when frame_size > INLINE_SLOTS */
    LValue *slots = nullptr;
    int inline_count = 0;           /* # slots placement-built in inline_buf */
    int size = 0;                   /* # valid (constructed) slots - the bound
                                     * the ML_VM_CHECK slot guard enforces */

    /* Lazily-allocated pure-call result cache (see PureCache above); null until
     * the first cacheable call is made from this frame. Freed with the frame. */
    std::unique_ptr<PureCache> pure_cache;

    Frame() = default;
    Frame(const Frame &) = delete;  /* never copied; slots would dangle */
    Frame(Frame &&) = delete;

    PureCache &ensure_pure_cache()
    {
        if (!pure_cache)
            pure_cache = std::unique_ptr<PureCache>(new PureCache());
        return *pure_cache;
    }

    /* Make `slots` point at storage holding exactly `frame_size` slots. */
    /*
     * Bounds-checked slot access. Under VM hardening (debug + CI-release) a
     * slot index >= size (a codegen bug that would otherwise read an
     * out-of-range LValue - a garbage type pointer, a layout-dependent crash)
     * fails LOUDLY here instead. Compiles to a plain `slots[i]` in a normal
     * release build. */
    ML_ALWAYS_INLINE LValue &at(int_type i)
    {
        ML_VM_CHECK(i >= 0 && i < size);
        return slots[i];
    }

    void init(int frame_size)
    {
        ML_CHECK(frame_size >= 0);
        size = frame_size;

        if (frame_size > INLINE_SLOTS) {

            /* Rare: too big to fit inline, one heap allocation. */
            heap_buf.resize(frame_size);
            slots = heap_buf.data();

        } else {

            slots = reinterpret_cast<LValue *>(inline_buf);

            for (int i = 0; i < frame_size; i++) {
                new (&slots[i]) LValue();
            }

            inline_count = frame_size;
        }
    }

    ~Frame()
    {
        /* Destroy only the inline slots we placement-constructed; heap_buf
         * (if used) destroys its own elements. */
        for (int i = 0; i < inline_count; i++) {
            slots[i].~LValue();
        }
    }
};

/*
 * The program-wide GLOBAL table: every top-level (named) function AND every
 * top-level variable that some function reads ("escaped"). Each gets a STATIC
 * slot index at compile time (the resolver), so a reference resolves to
 * SymKind::global + that index and reads this table from any call depth - the
 * single mechanism for all global symbol access (no scope-chain map walk for a
 * user global). A plain vector, sized once to the static global count and never
 * grown (no slot limit). A slot is `defined` only after its decl executes
 * (functions are hoisted, and a var's value is bound when its decl runs), so a
 * reference reaching a symbol before its definition runs reads as undefined.
 * Lexical reachability is a COMPILE-TIME decision (an out-of-scope name simply
 * never resolves to a slot). A top-level var that NO function reads stays a
 * main-frame local slot instead (so auto-const, which only sees frame slots, is
 * untouched). Despite the name, it is the global VARIABLE table too.
 */
struct GlobalFuncTable {
    std::vector<LValue> slots;
    std::vector<char> defined;   /* 1 once the decl has bound the slot */
    /* slot -> interned name; lets reflection (globals()) enumerate the table,
     * which is otherwise index-keyed. Not used on the hot read path. */
    std::vector<const UniqueId *> names;

    void init(const std::vector<const UniqueId *> &nm) {
        names = nm;
        slots.resize(nm.size());
        defined.assign(nm.size(), 0);
    }
};

class EvalContext {

    typedef std::map<const UniqueId *, LValue> SymbolsType;
    SymbolsType symbols;
    FlowState flow_state;   /* used only when this context is a flow root */

public:

    EvalContext *const parent;
    const bool const_ctx;
    const bool func_ctx;

    /* REPL mode (inherited). REPL: names map-resident (live map). SCRIPT: false,
     * the map must stay EMPTY (everything slotted) - asserted in emplace/lookup. */
    const bool repl_mode;

    /* True if this context OR any ancestor is a const-eval context. The map may
     * legitimately be written/read during compile-time folding: AutoConst
     * evaluates pure functions in throwaway non-const args contexts whose ROOT
     * is the const cctx, so a struct/func decl inside such a folded body
     * emplaces into a discarded map. Only a RUNTIME write (no const ancestor)
     * is the violation the empty-map invariant forbids. */
    bool in_const_eval() const {
        for (const EvalContext *c = this; c; c = c->parent)
            if (c->const_ctx) return true;
        return false;
    }

    /*
     * Transient: set true by handle_single_expr14 only while evaluating the
     * target of a *plain* assignment (`d[k] = v`), so a dict subscript/member
     * auto-vivifies a missing key (insert) instead of throwing. The outermost
     * Subscript/MemberExpr::do_eval consumes it (sets false) before recursing
     * into sub-expressions, so nested reads (`d[k1][k2]=v`: the `d[k1]` read)
     * still throw/default on a missing key. Reset after the target eval.
     */
    bool assign_target = false;

    /*
     * REPL only: when set on the persistent global scope, re-declaring an
     * existing name (`var x = ...` for a name already bound here) rebinds it
     * instead of throwing AlreadyDefinedEx - the documented way to change a
     * global's value/type at the prompt. Off (default) keeps the script rule
     * (a same-scope duplicate declaration is an error).
     */
    bool allow_redeclare = false;

    /*
     * The current call's slot Frame, or nullptr outside any resolved call.
     * Inherited from the parent on construction; do_func_call points a resolved
     * call's args context at a fresh Frame.
     */
    Frame *frame;

    /*
     * The program-wide global table - every top-level function AND every
     * escaped top-level variable - reachable from ANY call depth (inherited
     * from the parent; the root block owns it). Such a symbol is a
     * `SymKind::global` slot in here, so a global var read/write or a global /
     * recursive / mutually-recursive call is an O(1) table read instead of a
     * scope-chain map walk. nullptr when the program declares no such globals
     * (or in the REPL, where top-level names stay in the map).
     */
    GlobalFuncTable *gfuncs;

    /*
     * The current closure's capture vector (the called FuncObject's
     * `capture_slots`), or nullptr outside a closure body. Inherited from the
     * parent so nested blocks/loops in the body see it; do_func_call points it
     * at the called closure's vector. A `SymKind::capture` reference
     * reads/writes `(*captures)[slot]` - an O(1) slot, no map walk.
     */
    std::vector<LValue> *captures;

    /*
     * Points at the FlowState shared by every context within the current
     * function invocation. Function-boundary contexts (func_ctx) and the root
     * own their flow_state; nested blocks/loops inherit the parent's pointer.
     * Each call gets a fresh one, so recursion never shares flow state.
     *
     * NOT const: an InlinedCallExpr (a spliced block body) temporarily points
     * it at a stack-local FlowState to give the body its own return boundary,
     * then restores it - far cheaper than building a child EvalContext (which
     * carries a symbols-map member) per inlined call. That is the ONLY mutator.
     */
    FlowState *flow;

    EvalContext(const EvalContext &rhs) = delete;
    EvalContext(EvalContext &&rhs) = delete;

    EvalContext(EvalContext *parent = nullptr,
                bool const_ctx = false,
                bool func_ctx = false,
                bool repl = false);

    LValue *lookup(const Identifier *id);
    bool erase(const Identifier *id);

    void emplace(const Identifier *id, const EvalValue &val, bool is_const);
    void emplace(const Identifier *id, EvalValue &&val, bool is_const);
    void emplace(const std::string_view &id, EvalValue &&val, bool is_const);

    bool empty() const { return symbols.empty(); }
    void copy_symbols_from(const EvalContext &ctx) { symbols = ctx.symbols; }

    /* Append the interned name of every symbol bound in THIS context (not the
     * parent chain) to `out`. Used by the REPL completer to list globals +
     * builtins; a const-instance value is needed for member completion, so the
     * LValue is exposed too. */
    void collect_symbols(
        std::vector<std::pair<const UniqueId *, const LValue *>> &out) const;

    static SymbolsType builtins;
    static const SymbolsType const_builtins;
};

/*
 * The program-wide builtin table: a flat vector of every builtin's value, each
 * with a fixed index, built once (lazily) from const_builtins + builtins. A
 * builtin reference the resolver couldn't shadow with a user symbol resolves to
 * SymKind::builtin + its index, so it is an O(1) slot read - not a scope-chain
 * map walk. Entries are is_const-flagged (so an `aBuiltin = x` assignment to an
 * unshadowed builtin still raises CannotRebindBuiltinEx, and the shared global
 * table can't be corrupted). NOT used in the REPL (builtins stay map-resident
 * there so they remain redefinable). builtin_slot_index returns -1 if the name
 * is not a builtin.
 */
int builtin_slot_index(const UniqueId *uid);
LValue &builtin_slot(int index);
/* did the builtin come from const_builtins (visible during const-eval)? */
bool builtin_is_const(int index);

/*
 * DEV-ONLY builtins (the `show()` category): builtins that inherently need the
 * AST (e.g. show() decompiles it), so they are a DEV affordance - available in
 * the REPL and the test harness (both keep the AST), but RESERVED in an actual
 * script, where a call to one is a COMPILE-TIME error. This is what lets a
 * compiled script serialize to pure bytecode (no Construct*) while the dev tools
 * keep their AST access. `is_dev_builtin` tests membership (by interned name);
 * `g_dev_builtins_allowed` is the process-wide dev-harness flag (default false;
 * the -rt runner and the REPL set it true, a script leaves it false).
 */
bool is_dev_builtin(const UniqueId *uid);
extern bool g_dev_builtins_allowed;

/*
 * LAZY-ARG builtins (`defined`/`isconst`/`isconstdecl`): the argument is NOT
 * evaluated - a NODE property a runtime value can't reproduce - so calling one
 * INDIRECTLY (through a dyn value) is impossible to honor AST-free. In a
 * SCRIPT, using such a name as a VALUE (assigned / stored / passed - anything
 * but the callee of a direct call) is a COMPILE-TIME error (the inferencer's
 * reject_dev_builtins walk, same g_dev_builtins_allowed gate: the REPL keeps
 * the AST and allows the indirect form, like `show`). Maintainer-decided rule
 * (2026-07-14) - see plans/vm-fallback-elimination.md, fork F1.
 */
bool is_lazy_builtin(const UniqueId *uid);

/* The VALUE-read path of `base.member` (a struct field / const / dict key /
 * optional-none), shared by MemberExpr::do_eval and the VM's MemberV. */
class MemberExpr;
EvalValue member_read(const EvalValue &base, const MemberExpr *m);
/* The AST-free core: the VM's MemberV passes a member-key pool entry's fields
 * (the member_read wrapper passes a MemberExpr's). */
EvalValue member_read_core(const EvalValue &base, const EvalValue &memId,
                           const UniqueId *memUid, bool optional,
                           const Loc &mstart, const Loc &mend,
                           const Loc &bstart, const Loc &bend);

/* VM Phase 2b: append a POD struct built from the pre-evaluated ctor arg values
 * `vals` into `target` (a flat array<Struct> - coerced into the bytes - or a
 * general array - fallback build+append). AST-FREE: `cdef` is the ctor's POD
 * def, `flocs` the per-field coerce carets, `a0s`/`a0e` the container arg's
 * caret (the lvalue/array errors) - the VM passes them from its emplace_sites
 * pool entry. See eval.cpp. */
class CallExpr;
EvalValue vm_emplace_struct(EvalContext *ctx, LValue *target,
                            Loc a0s, Loc a0e, const StructTypeDef *cdef,
                            const ArgLoc *flocs, const EvalValue *vals,
                            size_t n);

/* F1 step 2: the VALUES dispatch of an INDIRECT builtin call, keyed on
 * Builtin::Kind - value (func_v), lvalue (func_lv; arg0 may be an
 * LValue*-boxed value, the by-ref encoding), map/filter (EAGER-ARGS by
 * language rule, via the shared vm_map_filter core), lazy/node (a tripwire -
 * script-unreachable). Shared by the tree-walker's indirect map/filter branch
 * and the VM's CallValueGenericV, so both engines agree. See eval.cpp. */
EvalValue dispatch_builtin_values(EvalContext *ctx, const Builtin &b,
                                  const ArgLocs *al, const EvalValue *args,
                                  size_t n);

/* The VALUES twin of construct_struct for an INDIRECT construction (args
 * pre-evaluated + unvalidated: the full runtime arity/coerce checks run, with
 * the pooled per-arg carets). See eval.cpp. */
EvalValue construct_struct_v(StructTypeDef *def, const ArgLocs *al,
                             const EvalValue *args, size_t nargs);

/* Shared call DISPATCH for an already-evaluated callee value (the VM's
 * CallValueGenericV + the tree-walker's CallExpr::do_eval): FuncObject /
 * Builtin / struct descriptor / non-callable, byte-identical. `ck`/`pc` +
 * `as_signal` route a FuncObject body's cross-frame VM exception (null / false
 * for the tree-walker). See eval.cpp. */
struct Chunk;
EvalValue dispatch_call_value(EvalContext *ctx, const EvalValue &callable,
                              const CallExpr *node, const Chunk *ck,
                              size_t pc, bool as_signal);

/* VM StructCtorV: construct a POD struct standalone `P(x, y)` from its field
 * VALUES (a register run). Only emitted for a typed-scalar-arg POD
 * construction, so coerce cannot throw here. See eval.cpp. */
intrusive_ptr<StructObject>
construct_struct_from_values(StructTypeDef *def,
                             const EvalValue *vals, size_t n);

/* VM StructCtorBoxedV: construct a BOXED (non-POD) struct from its field VALUES
 * (`vals[0..nargs)`), mirroring construct_struct's boxed loop. A field coerce
 * CAN throw (a dyn-laundered wrong value); `locs[i]` gives that arg's caret
 * (null == use empty locs). Omitted trailing opt fields (nargs < nfields) bind
 * none. See eval.cpp. */
intrusive_ptr<StructObject>
construct_struct_boxed_from_values(StructTypeDef *def, const EvalValue *vals,
                                  size_t nargs, const ArgLoc *locs);

/* VM MakeStructArrayV: build a FLAT array<PodStruct> literal from N structs'
 * field VALUES (interleaved: struct i's field j at vals[i*M + j], M =
 * def->fields.size()). Coerces straight into a contiguous flat byte buffer -
 * NO intermediate StructObject per element - then builds the mode-5 flat array.
 * Only emitted for all-scalar-field POD ctors, so coerce cannot throw. */
EvalValue vm_make_struct_array(StructTypeDef *def, size_t n,
                               const EvalValue *vals);

/* VM P2/P4: native subscript element store `c[k] = v` / `c[k] OP= v` for a dict
 * or a general array (any base - type-dispatched Type::subscript(for_write)
 * + slot_rmw). AST-free: the not-an-lvalue caret is a Loc pair (from the loc
 * side table), not a node. See eval.cpp. */
EvalValue vm_subscript_store(LValue *base_lv, const EvalValue &key,
                             const EvalValue &value, Op op,
                             Loc lstart, Loc lend);

/* VM (IncDecElemCheckedV): `c[k]++` / `c[k]--` on a dyn/unproven base - forms
 * the element LValue, enforces int/float, applies ±1 (statement). Two carets:
 * subscript loc (KeyNotFound/OOB) vs inc-dec loc (NotLValue/TypeError). See
 * eval.cpp. */
void vm_incdec_elem(LValue *base_lv, const EvalValue &key, bool is_inc,
                    Loc sub_start, Loc sub_end, Loc id_start, Loc id_end);

/* VM (IncDecMemberCheckedV): `d.f++` / `d.f--` on a dyn/unproven base - forms
 * the member LValue (struct field / dict value), enforces int/float, applies
 * ±1 (statement). Two carets: member loc (KeyNotFound) vs inc-dec loc
 * (NotLValue/TypeError). See eval.cpp. */
void vm_incdec_member(LValue *base_lv, const EvalValue &memId,
                      const UniqueId *memUid, bool is_inc,
                      Loc mstart, Loc mend, Loc id_start, Loc id_end);

/* VM (CoerceNumV): the typed-store numeric coerce - the same
 * coerce_to_decl_type the tree-walker's op==assign path runs (widen float <-
 * int/bool and int <- bool, pass none, THROW on a non-fitting dyn value).
 * See eval.cpp. */
EvalValue vm_coerce_decl_num(const EvalValue &v, bool is_float);

/* Form the member LValue* of `dval.member` for a ROOTED base (a mutable boxed
 * struct field / a dict value); nullptr for a POD field / readonly / non-
 * struct-non-dict (a value read). Shared by vm_incdec_member and the VM's
 * StoreLValueChainV walk. See eval.cpp. */
LValue *vm_member_lvalue_ref(const EvalValue &dval, const EvalValue &memId,
                             const UniqueId *memUid, bool for_write,
                             Loc mstart, Loc mend);

/* AST-shape purity check (id / scalar-literal / subscript / member / arith
 * chains only) - the gate try_flat_subscript_store / try_pod_struct_store key
 * on. Exported for the codegen's IncDecChainV allow_flat/allow_pod flags,
 * which must encode the tree-walker's OWN compile-shape-dependent semantics.
 * See eval.cpp (no_side_effects). */
bool construct_no_side_effects(const Construct *c);

/* VM IncDecChainV's FINAL-step semantics: given the walked-to container ref
 * `cur` (an LValue* wrapper or a plain VALUE - the StoreLValueChainV walk
 * convention) and the final member/subscript step, run IncDecExpr::do_eval's
 * exact tier logic and return the expression's value (old for postfix, new
 * for prefix). Tier 2 (`tier2`, a proven int/float lvalue) mirrors
 * handle_single_expr14's compound `±= 1` - the flat/POD fast stores only when
 * the codegen-proven allow_flat/allow_pod gate holds (try_flat/try_pod's
 * no_side_effects AST gate) - then derives old = new ∓ 1 with NO re-read;
 * tier 3 (dyn) mirrors the checked read-modify-write (NotLValue / const /
 * TypeError at the INC-DEC caret `id_*`). See eval.cpp. */
EvalValue vm_incdec_final(EvalValue &cur, bool is_member,
                          const EvalValue &memId, const UniqueId *memUid,
                          const EvalValue &key,
                          bool tier2, bool is_inc, bool is_prefix,
                          bool allow_flat, bool allow_pod,
                          Loc lstart, Loc lend, Loc kstart, Loc kend,
                          Loc id_start, Loc id_end);

/* VM (StoreMemberV): native `s.member = v` / `OP= v` for a STRUCT base (a dict
 * member store goes through DictStore). See eval.cpp. */
EvalValue vm_member_store(LValue *base_lv, const UniqueId *memUid, Op op,
                          const EvalValue &value,
                          const Loc &mstart, const Loc &mend,
                          const Loc &bstart, const Loc &bend);

/* The boxed field LValue* of `base.member` for a mutating builtin arg0
 * (`append(s.f, x)` — CallBuiltinLVMember). See eval.cpp. */
LValue *vm_member_lvalue(LValue *base_lv, const UniqueId *memUid,
                         const Loc &mstart, const Loc &mend,
                         const Loc &bstart, const Loc &bend);

/* VM (StoreElem2V): native NESTED store `a[i][j] = v` / `OP= v`. `locs[0]`/
 * `locs[1]` are the inner/outer subscript carets (deref only on a throw). See
 * eval.cpp. */
EvalValue vm_nested_subscript_store(LValue *outer_base, const EvalValue &key1,
                                    const EvalValue &key2,
                                    const EvalValue &value, Op op,
                                    const std::pair<Loc, Loc> *locs);

/* VM (StoreElemChainV): GENERIC N-level nested store `a[k0][k1]...[kn] = v` /
 * `OP= v`; `keys` + `steplocs` are base-to-innermost (steplocs[k] is that
 * subscript's caret). See eval.cpp. */
EvalValue vm_subscript_chain_store(LValue *base, const EvalValue *keys,
                                   size_t nkeys, const EvalValue &value, Op op,
                                   const std::pair<Loc, Loc> *steplocs);

/* VM (MapFilterV) + tree-walker shared map/filter core: apply a (pre-validated)
 * function to each element of a container. See eval.cpp. */
EvalValue vm_map_filter(EvalContext *ctx, const EvalValue &func_val,
                        const EvalValue &container, bool is_filter,
                        Loc cstart, Loc cend);

/* VM (ForeachDynNext): read element `i` of the array VALUE box-free (boxing a
 * flat scalar), reaching arr_elem_at in its own TU. See generic.cpp.h. */
EvalValue vm_arr_elem(const EvalValue &arr_val, size_type i);

/* VM LoadStructFieldInt/Float: read scalar field #fidx of element `idx` of a
 * flat array<PodStruct> directly from the bytes (the struct-foreach direct
 * read). No bounds/type checks - the codegen proved them. See eval.cpp. */
int_type vm_struct_field_int(const EvalValue &arrv, int_type idx,
                             int_type fidx);
float_type vm_struct_field_float(const EvalValue &arrv, int_type idx,
                                 int_type fidx);

/* Materialize element `idx` of a flat array<PodStruct> as a fresh StructObject
 * (the VM's LoadStructElemV - the whole-`p` foreach bind). See eval.cpp. */
EvalValue vm_struct_elem(const EvalValue &arrv, int_type idx);

/* The stored value of a PRESENT dict key, else nullptr (a plain map find - the
 * tree-walker's typed dict fast path). Shared by Subscript/MemberExpr eval_int/
 * eval_float and the VM's P3 typed dict reads (DictLoadInt/Float). */
const EvalValue *dict_present_value(const intrusive_ptr<DictObject> &obj,
                                    const EvalValue &key);

enum class ArrHint : unsigned char;   /* defined in syntax.h */

/* Build an array VALUE from `n` already-evaluated element values, honoring the
 * flat/general storage hint. Shared by the tree-walker (LiteralArray::do_eval)
 * and the bytecode VM's MakeArrayV op (which reads the values from registers),
 * so both build byte-identically. `is_const` == the elements are read-only. */
EvalValue build_array_from_values(const EvalValue *vals, size_t n,
                                  ArrHint hint,
                                  const StructTypeDef *hint_struct,
                                  bool is_const);

/* The value a baked const array/dict/struct LiteralObj materializes (immutable
 * share vs mutable clone, plus the general/flat_s arr_hint cases). Shared by
 * tree-walker (LiteralObj::do_eval) and the VM's LoadLiteralObjV op, so both
 * materialize byte-identically. */
EvalValue eval_literal_obj(const EvalValue &value, bool immutable,
                           ArrHint arr_hint,
                           const StructTypeDef *arr_hint_struct);

/* Build a dict VALUE from `npairs` INTERLEAVED key/value values ([k0,v0,..]).
 * Keys are frozen. Shared by LiteralDict::do_eval and the VM's MakeDictV. */
EvalValue build_dict_from_pairs(const EvalValue *pairs, size_t npairs,
                                bool is_const);

/* Inlining cost-model calibration: measure per-node-type eval cost from
 * hand-built AST nodes and print the weights. Driven by `--weights`. */
void run_weight_bench();

/* The per-frame pure-call cache (PureCache) is on by default; `-npc` clears
 * this to measure the recursion unroll without it. */
extern bool g_pure_cache_enabled;


inline EvalContext *
get_root_ctx(EvalContext *ctx)
{
    while (ctx->parent)
        ctx = ctx->parent;

    return ctx;
}

class FuncObject : public RefCounted {

public:

    const FuncDeclStmt *const func;
    /*
     * Per-instance storage for captured outer variables (an explicit `[x,y]`
     * capture list), filled once at closure creation in declaration order. A
     * body reference to a captured name resolves to SymKind::capture + its
     * index here, so it is an O(1) slot read - not a map walk. It lives in the
     * FuncObject (NOT the per-call Frame) because a mutable-by-value capture
     * must persist across calls to the same closure (e.g. a counter); each
     * closure instance / clone owns its own vector.
     */
    std::vector<LValue> capture_slots;
    /*
     * An empty context parented to the program root - the body's args context
     * parents to this, so the body reaches the global table (gfuncs) and the
     * builtins map. Holds no captured values (those are in capture_slots).
     */
    EvalContext capture_ctx;

    FuncObject(const FuncDeclStmt *func, EvalContext *ctx);
    FuncObject(const FuncObject &rhs);
};

/*
 * Deep, read-only copy of a const-evaluated array/dict value (see eval.cpp).
 * Used by the parser to bake a `const`-decl target into a LiteralObj that can
 * be shared (it can't be mutated) instead of deep-copied on every evaluation.
 */
EvalValue make_const_clone(const EvalValue &v);

/*
 * Mutable copies of an array/dict value (scalars/strings returned as-is):
 *  - make_mutable_clone: fresh mutable top, but read-only (const-backed)
 *    sub-objects are shared as-is. Backs the per-eval copy a `var`-bound
 *    materialized value needs, and keeps clone() shallow w.r.t. consts.
 *  - make_deep_mutable_clone: every level copied and made mutable (read-only
 *    dropped) - a fully independent writable value. Backs deepclone().
 */
EvalValue make_mutable_clone(const EvalValue &v);
EvalValue make_deep_mutable_clone(const EvalValue &v);

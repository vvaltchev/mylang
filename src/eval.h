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
    void init(int frame_size)
    {
        ML_CHECK(frame_size >= 0);

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

/* Inlining cost-model calibration: measure per-node-type eval cost from
 * hand-built AST nodes and print the weights. Driven by `--weights`. */
void run_weight_bench();


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

/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "uniqueid.h"

#include <map>
#include <new>
#include <string_view>

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
 * that call (EvalContext::frame is inherited from the parent). `live` is a
 * bitmask of which slots are currently defined - needed because undef() can
 * remove a binding and slots cannot hold the UndefinedId sentinel (LValue
 * forbids it). Slot count is capped at 64 so the mask fits a single word.
 *
 * The Frame lives on do_func_call's C++ stack. For the common case
 * (frame_size <= INLINE_SLOTS) its slots are placement-constructed into an
 * inline raw buffer, so a resolved call allocates nothing on the heap; only an
 * unusually large frame spills to the heap vector. Crucially init() constructs
 * EXACTLY frame_size slots (not INLINE_SLOTS), so a 1-slot frame pays for one
 * slot, not eight - the unused inline capacity is just stack bytes. `slots`
 * always points at whichever storage is active, so callers just index slots[i].
 */
struct Frame {
    static constexpr int INLINE_SLOTS = 8;

    alignas(LValue) char inline_buf[INLINE_SLOTS * sizeof(LValue)];
    std::vector<LValue> heap_buf;   /* spill when frame_size > INLINE_SLOTS */
    LValue *slots = nullptr;
    int inline_count = 0;           /* # slots placement-built in inline_buf */
    uint64_t live = 0;

    Frame() = default;
    Frame(const Frame &) = delete;  /* never copied; slots would dangle */
    Frame(Frame &&) = delete;

    /* Make `slots` point at storage holding exactly `frame_size` live slots. */
    void init(int frame_size)
    {
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

class EvalContext {

    typedef std::map<const UniqueId *, LValue> SymbolsType;
    SymbolsType symbols;
    FlowState flow_state;   /* used only when this context is a flow root */

public:

    EvalContext *const parent;
    const bool const_ctx;
    const bool func_ctx;

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
     * The current call's slot Frame, or nullptr outside any resolved call.
     * Inherited from the parent on construction; do_func_call points a resolved
     * call's args context at a fresh Frame.
     */
    Frame *frame;

    /*
     * Points at the FlowState shared by every context within the current
     * function invocation. Function-boundary contexts (func_ctx) and the root
     * own their flow_state; nested blocks/loops inherit the parent's pointer.
     * Each call gets a fresh one, so recursion never shares flow state.
     */
    FlowState *const flow;

    EvalContext(const EvalContext &rhs) = delete;
    EvalContext(EvalContext &&rhs) = delete;

    EvalContext(EvalContext *parent = nullptr,
                bool const_ctx = false,
                bool func_ctx = false);

    LValue *lookup(const Identifier *id);
    bool erase(const Identifier *id);

    void emplace(const Identifier *id, const EvalValue &val, bool is_const);
    void emplace(const Identifier *id, EvalValue &&val, bool is_const);
    void emplace(const std::string_view &id, EvalValue &&val, bool is_const);

    bool empty() const { return symbols.empty(); }
    void copy_symbols_from(const EvalContext &ctx) { symbols = ctx.symbols; }

    static SymbolsType builtins;
    static const SymbolsType const_builtins;
};


inline EvalContext *
get_root_ctx(EvalContext *ctx)
{
    while (ctx->parent)
        ctx = ctx->parent;

    return ctx;
}

class FuncObject {

public:

    const FuncDeclStmt *const func;
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

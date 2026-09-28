/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * #84: the INLINE half of VmInvoker's scalar callback path - the raw
 * bind and the test() hot path (sort's comparator, filter's predicate,
 * find's key), included by types.cpp (every test() caller is a builtin
 * there) and by vm.cpp (call_scalars binds through bind_raw too).
 *
 * WHY A HEADER: the hot path used to be ONE out-of-line function per
 * element (call_scalars_test) wrapping the fragment call - its prologue,
 * epilogue and argument marshalling were ~25 of the ~83 instructions a
 * comparison paid around a ~28-instruction comparator. Inlined into the
 * builtin's loop, the element pays exactly one call: the fragment's.
 * ⛔ STILL ONE CALL, AND IT MUST STAY ONE: vm_invoker_run's comment
 * records +19% wall clock at -27% Ir when an element paid TWO calls.
 * The uncommon exits (a non-boundary return, a pending raise, a release
 * scan, a result neither bool nor int) leave through test_tail /
 * test_result, out of line, on those paths only.
 */
#pragma once

#include "vm.h"
#include "eval.h"
#include "jit.h"

/* #97: the RAW scalar bind - see the CbScalar comment in vm.h */
/*
 * #97 CB6: no per-slot raw_bindable() test. Between two calls of a live
 * invoker EVERY window slot is trivial and not borrowed: the window is
 * pushed fresh, each call ends in the release scan over the list its
 * bind needs (ref_slots / ref_slots_raw), a slot outside that list can
 * never hold a reference, and nothing else writes the window between
 * elements (a throw unwinds the builtin, and the dtor pops it). That is
 * exactly what the VM_HARDENING audit at the end of vm_invoker_body
 * asserts over the whole window, so it is restated here as a check, not
 * paid as a test. The boxed decline lives out of line so this hot
 * function saves fewer registers.
 */
ML_ALWAYS_INLINE void VmInvoker::bind_raw(const CbScalar *ra, size_t n)
{
    LValue *win = w_->slots;
#if ML_VM_HARDENING
    for (size_t i = 0; i < n; i++)
        ML_VM_CHECK(win[i].raw_bindable());
#endif
#ifdef TESTS
    g_invoke_prepared++;
    g_invoke_raw++;
#endif
    for (size_t i = 0; i < n; i++) {
        /* #84 step 2: payload + type only - the window slot's tail is
         * already clear between elements (bind_scalar_payload) */
        if (ra[i].kind == 0)
            win[i].bind_scalar_payload(ra[i].i);
        else if (ra[i].kind == 1)
            win[i].bind_scalar_payload(ra[i].f);
        else
            win[i].bind_scalar_payload(ra[i].i != 0);
    }
}


inline bool VmInvoker::test_scalars(const CbScalar *ra, size_t n)
{
    /* the gates of call_scalars_test, plus a body that STARTS native -
     * anything else takes the old out-of-line path unchanged */
    if (!raw_ok_ || n != nparams_ || !entry_)
        return call_scalars_test(ra, n);
    bind_raw(ra, n);
    FlowState *fl = c_->flow;
    fl->type = FlowState::none;
#ifdef TESTS
    g_jit_invoke_direct++;
    g_invoke_test_inline++;
#endif
    /* ONE call for both kinds - jit_enter2 is jit_enter that also hands
     * back RDX (garbage unless the chunk has ret_truth_regs) */
    const JitRet2 rr = jit_enter2(entry_, w_->slots);
    /* (size_t)-2 is JIT_RET_BOUNDARY (vm.cpp): the body returned to us */
    if (rr.r != static_cast<size_t>(-2) || g_vm_exc_pending
            || !cck_->ref_slots_raw.empty())
        return test_tail(rr.r);
#if ML_VM_HARDENING
    for (int i = 0; i < static_cast<int>(w_->size); i++)
        ML_VM_CHECK(w_->slots[i].get().get_type()->t < Type::t_str);
#endif
    if (cck_->ret_truth_regs) {
        /* #84 step 3: every return is an int or bool whose payload is
         * its truth, left in rdx by the boundary return (Chunk::
         * ret_truth_regs) - no flow->value read */
#ifdef TESTS
        g_invoke_test_regs++;
#endif
        fl->type = FlowState::none;
        return rr.pay != 0;
    }
    if (fl->type != FlowState::ret)
        return false;                     /* no value: none, falsy */
    const Type *t = fl->value.get_type();
    if (t->t == Type::t_bool) {
        fl->type = FlowState::none;
        return fl->value.raw_bval();
    }
    if (t->t == Type::t_int) {
        fl->type = FlowState::none;
        return fl->value.raw_ival() != 0;
    }
    return test_result();                 /* a float, a reference, ... */
}

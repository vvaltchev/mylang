/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#define _USE_MATH_DEFINES
#include <iostream>
#include <memory>
#include <utility>
#include <cstdint>

/*
 * The following types and funcs are by far too common to be used with the
 * std:: prefix. Therefore, we're selectively enabling them in our global
 * namespace.
 */
using std::cin;
using std::cout;
using std::cerr;
using std::ostream;
using std::endl;

using std::unique_ptr;
using std::shared_ptr;
using std::make_pair;
using std::make_shared;
using std::make_unique;

/* Custom type defs */
/*
 * The script float type. `double` (not `long double`): it keeps EvalValue small
 * (long double's 16-byte alignment padded EvalValue from 40 to 48 bytes, which
 * dominated array memory traffic and every value copy), matches Python's float
 * exactly, and uses the much faster double libm. If you change this, update the
 * printf/snprintf format strings (currently "%f"/"%.*f") and the math builtins.
 */
typedef double float_type;
typedef intptr_t int_type;

#ifndef _MSC_VER

   /*
    * Using 32-bit unsigned as size/offset type is slightly more efficient
    * than using size_t on 64-bit machines because:
    *
    *      1. Makes the structures smaller (4 bytes vs. 8 bytes)
    *      2. On x86_64, it doesn't require the extra REX.W prefix
    *
    * Overall, we don't really need to handle arrays or strings larger than 4 GB
    * in this simple language. Better take advantage of that.
    */
    typedef uint32_t size_type;

#else

    /*
     * Microsoft's compiler complains too much about integer truncation.
     * While the "problem" is exactly the same with GCC and Clang, and the
     * trade-off is just to support at most 2^32 elements but having smaller
     * EvalValue objects (=> faster to copy), compiling with this compiler
     * WITHOUT warnings, requires a *ton* of extra casts. Therefore, just 
     * use size_t and make the compiler happy.
     */
    typedef size_t size_type;
#endif

/*
 * Force-inline: `inline` is only a hint, and the compiler drops it for a tiny
 * helper once its CALLER grows too large (its inline budget shrinks). The VM's
 * dispatch loop (vm_run_chunk) is exactly that case - it grows as opcodes are
 * added, and past a threshold the compiler stopped inlining the hot per-op
 * operand helpers (read_int_operand/write_int_slot/...), adding a call+return
 * to EVERY int/float op and regressing the whole VM ~11% (measured). Marking
 * those leaf helpers ML_ALWAYS_INLINE pins them into the loop regardless of its
 * size. Use ONLY for tiny, hot, non-recursive, non-address-taken leaves.
 */
#if defined(_MSC_VER)
#  define ML_ALWAYS_INLINE __forceinline
#else
#  define ML_ALWAYS_INLINE inline __attribute__((always_inline))
#endif

/*
 * The opposite: keep a COLD path (rare error / big-arity fallback) OUT of a hot
 * caller, so its setup/teardown code (a std::vector ctor+dtor, etc.) does not
 * bloat the caller and push the caller past the inline threshold above.
 */
#if defined(_MSC_VER)
#  define ML_NOINLINE __declspec(noinline)
#else
#  define ML_NOINLINE __attribute__((noinline))
#endif

/*
 * A COLD, never-inlined helper for a RARE path (a runtime error, a rare op's
 * body). Beyond ML_NOINLINE, the `cold` attribute tells GCC/clang the CALL SITE
 * is unlikely, so the block that calls it is moved OUT-OF-LINE (a cold section)
 * - which DENSIFIES a big hot dispatch function (vm_run_chunk): the rare-op case
 * bodies stop pushing the hot int/loop handlers apart, the layout regression the
 * growing switch caused (see [[vm-dispatch-frontend-regression]]). Attributes
 * only - no logic change. (MSVC has no equivalent; NOINLINE is the fallback.)
 */
#if defined(_MSC_VER)
#  define ML_COLD __declspec(noinline)
#else
#  define ML_COLD __attribute__((cold, noinline))
#endif

/*
 * ML_CHECK / ML_CHECK_MSG - the project's defense-in-depth assertion macros.
 *
 * Use these (NOT bare assert) to state an invariant the code RELIES ON but a
 * caller could violate with an incorrect change: "this can't happen if the
 * code is correct." They are the Swiss-cheese layers - each one is a hole in a
 * different place, so a bad change has many chances to hit a wall. They must be
 * SIDE-EFFECT FREE (the condition is compiled out in a plain release build).
 *
 * Tied to the build's ASSERTS flag, exactly like the C `assert()`: active
 * unless NDEBUG is defined. ASSERTS defaults ON for EVERY build type (debug and
 * release alike); a build with ASSERTS=0 defines NDEBUG, which compiles both
 * the C asserts and these away (the way to measure the assert overhead, e.g.
 * `make OPT=1 ASSERTS=0`). So all CI runs exercise the full net.
 *
 * For conditions that CAN legitimately occur at runtime (bad user input, I/O
 * failure, a real type error) do NOT use these - throw a proper Exception so
 * the error is handled in every build. ML_CHECK is for "impossible" states.
 */
#ifndef NDEBUG
#  include <cassert>
#  define ML_CHECK(cond)          assert(cond)
#  define ML_CHECK_MSG(cond, msg) assert((cond) && (msg))
#else
#  define ML_CHECK(cond)          ((void)0)
#  define ML_CHECK_MSG(cond, msg) ((void)0)
#endif

/*
 * ML_VM_CHECK - HEAVY per-op VM invariants (frame-slot bounds, ...): too hot
 * for a plain release (a compare on every register access), but invaluable for
 * exposing a LAYOUT-DEPENDENT UB - a bad slot index that silently reads an
 * unconstructed/out-of-range LValue (garbage type pointer) crashes only on some
 * toolchains, but the bounds check catches it EVERYWHERE, deterministically.
 *
 * Gated by ML_VM_HARDENING (and, like ML_CHECK, by NDEBUG). The BUILD sets the
 * default: ON in a debug build, OFF in a plain release (speed) - the Makefile /
 * CMake pass -DML_VM_HARDENING=0 for a release unless VM_HARDENING=1. CI turns
 * it ON in the RELEASE lanes, so a CI release runs with far more safety than a
 * local release. If undefined here, it defaults ON (fail safe).
 */
#ifndef ML_VM_HARDENING
#  define ML_VM_HARDENING 1
#endif
#if ML_VM_HARDENING
#  define ML_VM_CHECK(cond)          ML_CHECK(cond)
#  define ML_VM_CHECK_MSG(cond, msg) ML_CHECK_MSG(cond, msg)
#else
#  define ML_VM_CHECK(cond)          ((void)0)
#  define ML_VM_CHECK_MSG(cond, msg) ((void)0)
#endif

/*
 * ML_UNTRUSTED_CHECK - the PROVENANCE tier (#137, maintainer-set 2026-08-09).
 *
 * The axis that matters for a corrupt `.myv` is not the BUILD TYPE, it is
 * WHERE THE BYTECODE CAME FROM. Bytecode `vm_compile` just produced in this
 * process is correct by construction and covered by the whole test suite;
 * bytecode read off a disk is arbitrary bytes. Gating the defence on ASSERTS
 * conflates those, and gets it exactly backwards: the build that most needs
 * the check (an optimized, assert-free release running a shipped image) is
 * the one that loses it.
 *
 * So this tier is NOT compiled out by ASSERTS. It is a test of one global,
 * `g_untrusted_bytecode`, which is FALSE for a fresh compile and set only by
 * myv_read. A trusted run pays a predictable, never-taken branch on a
 * hot-cached global; an untrusted run pays the check.
 *
 * ⛔ USE IT ONLY where the condition is genuinely VALUE-DEPENDENT - where
 * verify_chunk cannot decide it at load from the image alone. Anything
 * statically decidable belongs THERE instead, at zero runtime cost (that is
 * tier 1; see verify_chunk in codegen.cpp). This tier is the residue.
 *
 * It THROWS rather than asserting: a corrupt image is input, and an input
 * error must be reported in every build, not aborted on in some.
 * `UNTRUSTED_CHECKS=0` compiles the whole tier away - the A/B lever for
 * measuring what the never-taken branch costs, NOT a shipping config.
 */
#ifndef ML_UNTRUSTED_CHECKS
#  define ML_UNTRUSTED_CHECKS 1
#endif
#if ML_UNTRUSTED_CHECKS
extern bool g_untrusted_bytecode;
[[noreturn]] void ml_untrusted_fail(const char *what);
#  define ML_UNTRUSTED_CHECK(cond, what)                                    \
      do {                                                                  \
          if (g_untrusted_bytecode && !(cond))                              \
              ml_untrusted_fail(what);                                      \
      } while (0)
#else
#  define ML_UNTRUSTED_CHECK(cond, what) ((void)0)
#endif

/*
 * The same provenance question as a PREDICATE, for the one place the
 * macro cannot serve: a helper the emitter takes NO status from, which
 * must therefore take a DEFINED FALLBACK rather than throw (#142). Such
 * a site still wants its ML_VM_CHECK tripwire for bytecode WE compiled -
 * where the condition really is an interpreter bug - while staying
 * silent for an image, where a corrupt value is input, not a bug. Spell
 * that `ML_VM_CHECK(ml_untrusted_bytecode() || <invariant>)`.
 * Constant-folds to `false` when the tier is compiled out.
 */
inline bool ml_untrusted_bytecode()
{
#if ML_UNTRUSTED_CHECKS
    return g_untrusted_bytecode;
#else
    return false;
#endif
}

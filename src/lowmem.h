/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

#if defined(__linux__) || defined(__FreeBSD__)
#  include <sys/mman.h>
#endif

/*
 * THE LOW-ADDRESS ARENA (#96) - a few objects placed below 2^31 so the
 * JIT can name them with a SIGN-EXTENDED imm32.
 *
 * WHY. x86-64's `mov qword [rbx+disp], imm32` sign-extends its
 * immediate, so it can store a pointer only if that pointer is
 * < 0x8000'0000. The JIT's type tags (`t_int`, `t_float`) are exactly
 * such pointers, written into a slot's type field constantly - and
 * because the binary is PIE they land around 0x6348'0000'0000, which
 * does not fit. That is the ONLY reason they were kept in registers
 * (rsi/r8), and since both are CALLER-saved the emitter re-materialised
 * them after every helper call: 108 `movabs rsi` and 106 `movabs r8` on
 * 09_fib_recursive, a program with no float arithmetic in it.
 *
 * ⛔ "LOW ABSOLUTE ADDRESS" IS THE REQUIREMENT, NOT "NEAR THE CODE".
 * Proximity buys RIP-relative addressing, which is a two-instruction
 * LOAD (`mov rax,[rip+d]; mov [slot],rax`) and therefore worse than the
 * pinned register it would replace. Only the low absolute address gives
 * the ONE-instruction store. (On AArch64 the property INVERTS -
 * proximity is what matters there, because ADR/ADRP are PC-relative and
 * there is no store-immediate at all. See plans/jit-registers.md.)
 *
 * WHAT THIS IS NOT. It is not a general allocator and must not become
 * one. The requirement is ~14 objects, constructed ONCE at static-init,
 * NEVER freed, a few hundred bytes total: no free list, no coalescing,
 * no size classes, no thread safety. A bump pointer over one mapping is
 * the whole design, which is why pulling in a third-party malloc would
 * be ~5000 lines to serve 30.
 *
 * PLATFORMS. `MAP_32BIT` exists on Linux and FreeBSD x86-64. Darwin has
 * no such flag AND gives 64-bit executables a 4GB `__PAGEZERO`, so no
 * low address is mappable there at all - but Darwin x86-64 will never
 * run native code (maintainer, 2026-08-17: Intel Macs are deprecated,
 * Apple Silicon only), and on Apple Silicon the tags live in
 * callee-saved x27/x28 instead. Everywhere the arena is unavailable
 * this returns null, the caller falls back to ordinary `new`, and the
 * JIT keeps the register form.
 *
 * ⛔ THE FALLBACK IS NOT DECORATION - IT IS REACHABLE ON LINUX TOO.
 * `MAP_32BIT` can fail at runtime: the low 2GB can be exhausted, a
 * hardened kernel can refuse, or the program itself may have mapped it.
 * So no caller may ASSUME a low pointer; the JIT tests each pointer
 * with `ml_lowmem_fits_imm32` and picks its encoding per store.
 */

#if (defined(__linux__) || defined(__FreeBSD__)) && defined(__x86_64__) \
    && defined(MAP_32BIT)
#  define ML_LOWMEM_SUPPORTED 1
#else
#  define ML_LOWMEM_SUPPORTED 0
#endif

/* Every byte of the arena must be < 2^31, so ANY pointer into it - not
 * merely its base - is imm32-encodable. */
inline bool ml_lowmem_fits_imm32(const void *p)
{
    return reinterpret_cast<uintptr_t>(p) < 0x80000000u;
}

/*
 * Bump-allocate `size` bytes with `align`; null when unavailable or
 * full. The mapping is made ONCE, on the first call, and never
 * unmapped - its contents are process-lifetime singletons.
 */
struct MlLowmemArena {
    char *cur = nullptr;
    char *end = nullptr;
    MlLowmemArena()
    {
#if ML_LOWMEM_SUPPORTED
        /*
         * ⛔ MYLANG_NO_LOWMEM=1 - REFUSE THE ARENA, so the fallback the
         * comment above calls "reachable on Linux too" is REACHABLE BY
         * A TEST. Without it that path is a shipping configuration
         * (Darwin, Windows, a hardened kernel, an exhausted low 2GB)
         * that no local build and no Linux CI lane can enter - and the
         * JIT behaves MATERIALLY differently in it: every type tag is a
         * register read rather than an imm32, which decides whether rsi
         * and r8 are pinnable at all (jit_xcache_busy). A branch only
         * one platform can take is a branch nobody tests; this is the
         * same reason the `lto0` CI lane exists.
         *
         * Read here and not at each use: the arena is built once at
         * static init, so one getenv cannot cost anything measurable,
         * and a per-use switch could flip mid-run and break the
         * invariant that ONE tested pointer speaks for the rest.
         */
        if (const char *s = getenv("MYLANG_NO_LOWMEM"))
            if (s[0] && s[0] != '0')
                return;
        /* One page is already ~4x what the type singletons need; the
         * arena deliberately does NOT grow, because a second mapping
         * could land high and silently break the invariant that a
         * caller may test ONE pointer and trust the rest. */
        const size_t len = 4096;
        void *p = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED)
            return;
        /* The LAST byte must fit too, not just the base. */
        if (!ml_lowmem_fits_imm32(static_cast<char *>(p) + len - 1)) {
            munmap(p, len);
            return;
        }
        cur = static_cast<char *>(p);
        end = cur + len;
#endif
    }
};

inline MlLowmemArena &ml_lowmem_arena()
{
    static MlLowmemArena a;
    return a;
}

/* Did the arena get its mapping? Lets a test tell "this platform has no
 * low memory" (fine) from "it does and we failed to use it" (a bug) -
 * a bare is-this-pointer-low check cannot separate those. */
inline bool ml_lowmem_available() { return ml_lowmem_arena().cur != nullptr; }

inline void *ml_lowmem_alloc(size_t size, size_t align)
{
    MlLowmemArena &a = ml_lowmem_arena();
    if (!a.cur || !align)
        return nullptr;
    const uintptr_t at = reinterpret_cast<uintptr_t>(a.cur);
    const uintptr_t aligned = (at + align - 1) & ~(uintptr_t)(align - 1);
    char *const p = reinterpret_cast<char *>(aligned);
    if (p + size > a.end)
        return nullptr;                  /* full: caller uses plain new */
    a.cur = p + size;
    return p;
}

/*
 * Construct T in the arena when it is available, else on the ordinary
 * heap. NEVER DESTROYED either way - these are process-lifetime
 * singletons, exactly as the plain `new`s this replaced were.
 */
template <class T, class... A>
inline T *ml_lowmem_new(A &&...args)
{
    if (void *p = ml_lowmem_alloc(sizeof(T), alignof(T)))
        return new (p) T(std::forward<A>(args)...);
    return new T(std::forward<A>(args)...);
}

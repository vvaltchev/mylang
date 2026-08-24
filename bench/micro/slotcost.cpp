/*
 * What does a MyLang frame-slot round-trip actually COST on this machine?
 *
 * Same arithmetic as bench/my/03_int_arith.my, three ways:
 *   R  - everything in locals (the compiler keeps them in registers)
 *   M  - each intermediate stored to AND reloaded from a frame slot, at
 *        MyLang's real 48-byte LValue stride
 *   MT - the same, plus the 8-byte Type* tag store MyLang also emits
 *
 * MT is what MyLang emits today; R is what a perfect register allocator
 * would leave. The gap between them is the allocator's ceiling on this
 * shape, measured rather than reasoned about.
 *
 * MEASURED 2026-08-15 (Intel Core Ultra 9 285T, g++ -O2, 20M iterations):
 *
 *   latency-bound  (the serial acc chain)    R 3.30 ns   MT 3.25 ns  1.00x
 *   throughput-bound (4 independent accums)  R 0.32 ns   M  1.03 ns  3.21x
 *
 * So a frame-slot round-trip is FREE in a loop whose own dependency
 * chain is long enough to hide it, and costs 3.2x in one with real
 * instruction-level parallelism. That is the whole answer to "what would
 * a register allocator buy": it depends entirely on which regime the
 * loop is in, and 03_int_arith - which does ELEVEN slot references per
 * iteration - is in the first one.
 *
 * Build:  g++ -O2 -std=c++17 -o slotcost slotcost.cpp && ./slotcost 20
 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <chrono>

static volatile int64_t *slots;   /* volatile: the round-trip must be real */
static void *g_int_tag;

__attribute__((noinline)) static int64_t run_R(int64_t n)
{
    int64_t acc = 1;
    for (int64_t i = 1; i < n; i++) {
        acc = (acc + i) * 3;
        acc = acc % 1000000007;
        acc = acc + i / 2 - i % 7;
    }
    return acc;
}

/* 48-byte LValue stride: slot k's payload lives at slots[k * 6]. */
#define S(k) slots[(k) * 6]

__attribute__((noinline)) static int64_t run_M(int64_t n)
{
    int64_t acc = 1;
    for (int64_t i = 1; i < n; i++) {
        S(4) = acc + i;                       /* temp 4 */
        acc = S(4) * 3;
        acc = acc % 1000000007;
        S(4) = i / 2;                         /* temp 4 again */
        S(5) = acc + S(4);                    /* temp 5 */
        S(6) = i % 7;                         /* temp 6 */
        acc = S(5) - S(6);
    }
    return acc;
}

__attribute__((noinline)) static int64_t run_MT(int64_t n)
{
    int64_t acc = 1;
    for (int64_t i = 1; i < n; i++) {
        *(void *volatile *)&slots[4 * 6 + 3] = g_int_tag;
        S(4) = acc + i;
        acc = S(4) * 3;
        acc = acc % 1000000007;
        *(void *volatile *)&slots[4 * 6 + 3] = g_int_tag;
        S(4) = i / 2;
        *(void *volatile *)&slots[5 * 6 + 3] = g_int_tag;
        S(5) = acc + S(4);
        *(void *volatile *)&slots[6 * 6 + 3] = g_int_tag;
        S(6) = i % 7;
        acc = S(5) - S(6);
    }
    return acc;
}

/* `n + r` per rep: run_R is PURE, so with a constant argument GCC hoists
 * all seven calls into one and six reps measure nothing (min -> 0.00). */

/*
 * THE CONTROL: no long serial chain. Four INDEPENDENT accumulators, so
 * the loop is throughput-bound rather than latency-bound. If the slot
 * round-trip is free here too, it is free generally; if it costs here,
 * then it was merely hiding in the shadow of the acc chain above.
 */
__attribute__((noinline)) static int64_t run_R_ilp(int64_t n)
{
    int64_t a = 0, b = 0, c = 0, d = 0;
    for (int64_t i = 1; i < n; i++) {
        a += i * 3;
        b += i ^ 5;
        c += i | 9;
        d += i & 17;
    }
    return a + b + c + d;
}

__attribute__((noinline)) static int64_t run_M_ilp(int64_t n)
{
    int64_t a = 0, b = 0, c = 0, d = 0;
    for (int64_t i = 1; i < n; i++) {
        *(void *volatile *)&slots[4 * 6 + 3] = g_int_tag;
        S(4) = i * 3;   a += S(4);
        *(void *volatile *)&slots[5 * 6 + 3] = g_int_tag;
        S(5) = i ^ 5;   b += S(5);
        *(void *volatile *)&slots[6 * 6 + 3] = g_int_tag;
        S(6) = i | 9;   c += S(6);
        *(void *volatile *)&slots[7 * 6 + 3] = g_int_tag;
        S(7) = i & 17;  d += S(7);
    }
    return a + b + c + d;
}

/*
 * ---------------------------------------------------------------------
 * #96: 64_struct_create's shape. Its emitted loop is 137 instructions of
 * which ~60 are TEN blocks of runtime float type-dispatch
 *
 *     mov rax, <slot>.type ; cmp rax, r8 ; jne cold
 *     movsd xmm0, <slot>   ; jmp done
 *   cold: cvtsi2sd xmm0, <slot>
 *
 * on values the emitter itself has just tagged. RS keeps the
 * intermediates in registers (what a perfect allocator leaves); MS
 * round-trips them through 48-byte slots WITH the tag store and the
 * dispatch (what ships). The gap is the ceiling for #96.
 * ---------------------------------------------------------------------
 */
static void *g_flt_tag;
static volatile double *fslots;

__attribute__((noinline)) static double run_struct_R(int64_t n)
{
    double fs = 0; int64_t sx = 0;
    for (int64_t i = 1; i < n; i++) {
        const int64_t px = i, py = i * 2;      /* Point(i, i*2) */
        sx += px + py;
        const double a = (double)i * 1.0;      /* Vec3(...) */
        const double b = (double)i * 2.0;
        const double c = (double)i * 3.0;
        fs += a + b + c;
    }
    return fs + (double)sx;
}

#define FS(k) fslots[(k) * 6]
#define FT(k) (*(void *volatile *)&fslots[(k) * 6 + 3])

/* one dispatched read: the tag load, the compare, the taken arm */
static inline double disp(int k)
{
    return FT(k) == g_flt_tag ? FS(k) : (double)(int64_t)FS(k);
}

__attribute__((noinline)) static double run_struct_M(int64_t n)
{
    double fs = 0; int64_t sx = 0;
    for (int64_t i = 1; i < n; i++) {
        FT(4) = g_flt_tag; FS(4) = (double)(i * 2);
        sx += i + (int64_t)FS(4);
        FT(5) = g_flt_tag; FS(5) = (double)i * 1.0;
        FT(6) = g_flt_tag; FS(6) = (double)i * 2.0;
        FT(7) = g_flt_tag; FS(7) = (double)i * 3.0;
        fs += disp(5) + disp(6) + disp(7);
    }
    return fs + (double)sx;
}

template <class F> static double best_of(F f, int64_t n, int reps);

template <class F> static double best_of(F f, int64_t n, int reps)
{
    double b = 1e18;
    for (int r = 0; r < reps; r++) {
        auto t0 = std::chrono::steady_clock::now();
        volatile int64_t sink = f(n + r);
        (void)sink;
        auto t1 = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        if (s < b) b = s;
    }
    return b;
}

int main(int argc, char **argv)
{
    const int64_t n = (argc > 1 ? atol(argv[1]) : 20) * 1000000 + 1;
    static int64_t buf[64];
    slots = buf;
    g_int_tag = (void *)0x1234;
    const double r  = best_of([](int64_t k) { return run_R(k);  }, n, 7);
    const double m  = best_of([](int64_t k) { return run_M(k);  }, n, 7);
    const double mt = best_of([](int64_t k) { return run_MT(k); }, n, 7);
    const double it = (double)(n - 1);
    printf("iterations       %.0f\n", it);
    printf("R   registers    %8.4f s   %6.2f ns/iter   1.00x\n",
           r, r / it * 1e9);
    printf("M   +slot rt     %8.4f s   %6.2f ns/iter   %.2fx\n",
           m, m / it * 1e9, m / r);
    printf("MT  +type tag    %8.4f s   %6.2f ns/iter   %.2fx\n",
           mt, mt / it * 1e9, mt / r);
    const double ri = best_of([](int64_t k) { return run_R_ilp(k); }, n, 7);
    const double mi = best_of([](int64_t k) { return run_M_ilp(k); }, n, 7);
    printf("\n-- CONTROL: no serial chain (4 independent accumulators) --\n");
    printf("R   registers    %8.4f s   %6.2f ns/iter   1.00x\n",
           ri, ri / it * 1e9);
    printf("M   +slot rt+tag %8.4f s   %6.2f ns/iter   %.2fx\n",
           mi, mi / it * 1e9, mi / ri);
    static double fbuf[64];
    fslots = fbuf;
    g_flt_tag = (void *)0x5678;
    const double rs = best_of([](int64_t k) { return run_struct_R(k); }, n, 7);
    const double ms = best_of([](int64_t k) { return run_struct_M(k); }, n, 7);
    printf("\n-- #96: 64_struct_create's shape (slots + tags + dispatch) --\n");
    printf("RS  registers    %8.4f s   %6.2f ns/iter   1.00x\n",
           rs, rs / it * 1e9);
    printf("MS  +slots+disp  %8.4f s   %6.2f ns/iter   %.2fx\n",
           ms, ms / it * 1e9, ms / rs);
    return 0;
}


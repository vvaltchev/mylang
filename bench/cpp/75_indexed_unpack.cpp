/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/75_indexed_unpack.my: build 50 rows of [name, val]
 * strings, then N times run an `indexed` foreach that unpacks (i, name, val)
 * and accumulates i + len(name) + len(val) (no intermediate modulo; mod only
 * at the end). Reproduced as a vector of (string,string) rows iterated by a
 * range-for with a running index. bench_sink_ptr blocks -O3 from hoisting the
 * loop-invariant inner reduction out of the outer loop (an O(1) collapse), so
 * the inner scan runs each of the N passes - matching the .my's per-pass work. */
#include "bench.h"

/* BENCH-FAIR (plans/archived/bench-fairness.md, class E - re-audited after the
 * 88.9x result was challenged): the C++ loop itself is honest (asm: 8
 * scalar instructions/row, re-run every rep - no hoisting, no SIMD), but
 * `const auto &row` binds by REFERENCE, which MyLang's unpack semantics
 * cannot do: `foreach (i, name, val in indexed rows)` STRICT-checks the
 * row length and binds name/val as REFCOUNTED HANDLE COPIES per row
 * (SharedStr handles - a ++/-- on the shared body each). This twin
 * mirrors those mechanics. refcnt is volatile so the compiler cannot
 * cancel the balanced ++/-- pair (MyLang's are real memory RMWs the
 * optimizer never sees as a foldable pair). */
struct StrBody {
    std::string s;
    volatile long refcnt = 1;
};
struct StrHandle {
    StrBody *b;
    StrHandle(StrBody *body) : b(body) { b->refcnt = b->refcnt + 1; }
    StrHandle(const StrHandle &o) : b(o.b) { b->refcnt = b->refcnt + 1; }
    ~StrHandle() { b->refcnt = b->refcnt - 1; }
    long len() const { return (long)b->s.size(); }
};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 100000L * scale;

    /* rows: array<array<str>> - shared string bodies, handle elements. */
    std::vector<StrBody *> bodies;
    std::vector<std::vector<StrHandle>> rows;
    for (long j = 0; j < 50; j++) {
        StrBody *a = new StrBody{"item" + std::to_string(j)};
        StrBody *b = new StrBody{std::to_string(j * 2)};
        bodies.push_back(a);
        bodies.push_back(b);
        rows.push_back({StrHandle(a), StrHandle(b)});
    }

    long acc = 0;
    for (long r = 0; r < N; r++) {
        bench_sink_ptr(rows.data());    /* block hoisting the reduction */
        long i = 0;
        for (const auto &row : rows) {
            if (row.size() != 2)        /* the STRICT unpack arity check */
                abort();
            StrHandle name = row[0];    /* refcounted handle bind */
            StrHandle val = row[1];     /* refcounted handle bind */
            acc += i + name.len() + val.len();
            i++;
        }
    }

    printf("result: %ld\n", acc % 1000000007);
    return 0;
}

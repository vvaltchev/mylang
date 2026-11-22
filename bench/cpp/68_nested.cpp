/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/68_nested.my: an auto-generated 15-deep nested
 * workload (for/while/if with break/continue) mutating an int array A[7], a
 * growable list G (capped at 300), a small dict D (keys 0..4), and three modular
 * accumulators s1/s2/s3. Transcribed brace-for-brace from the .my; MyLang's
 * for/while/if/break/continue map 1:1 to C++, and all arithmetic is int64 mod
 * 1e9+7 (no overflow: intermediates stay well under 2^63). Two inner blocks that
 * repeat verbatim in the source (the `for v2` body under the s1%4 if/else, and
 * the `for v10` body under the inner s1%4 if/else) are factored into by-ref
 * lambdas so the transcription can't drift between the identical copies. */
#include "bench.h"
#include <unordered_map>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long m = 1000000007;
    long A[7] = {1, 2, 3, 4, 5, 6, 7};
    std::vector<long> G;
    /* A real dict (hashmap), like MyLang's D = {0:0,1:0,2:0,3:0,4:0}: every
     * D[k%5] access hashes/probes it. operator[] default-inserts 0, giving the
     * same net values as the pre-initialized keys 0..4. */
    std::unordered_map<long, long> D;
    long s1 = 1, s2 = 2, s3 = 3;
    long reps = 5000 * scale;

    /* The `for v10` block (identical in both s1%4 branches, .my L43-66/L70-93). */
    auto run_v10 = [&]() {
        for (long v10 = 1; v10 < 4; v10 = v10 * 2) {
            s2 = (s2 + A[(s1 + 11) % 7]) % m;
            for (long v11 = 0; v11 < 2; v11++) {
                if (s3 % 11 == 0) continue;
                if ((long)G.size() < 300) G.push_back((s1 + s2) % 97);
                long v12 = 0;
                while (v12 < 2) {
                    v12 += 1;
                    D[13 % 5] = D[13 % 5] + (s1 % 3);
                    long v13 = 0;
                    while (v13 < 2) {
                        v13 += 1;
                        s3 = (s3 + v13 * v13 + 1) % m;
                        for (long v14 = 0; v14 < 2; v14++) {
                            if (s3 % 11 == 0) continue;
                            s1 = (s1 * 31 + v14 + 15) % m;
                            A[v14 % 7] = (A[v14 % 7] * 3 + s1 * 2 - s2 + 15
                                          + 1000000007) % m;
                            if (s2 % 13 == 0) break;
                        }
                    }
                }
                if (s2 % 13 == 0) break;
            }
        }
    };

    /* The `for v2` block (identical in both s1%4 branches, .my L18-105/L109-196). */
    auto run_v2 = [&]() {
        for (long v2 = 0; v2 < 2; v2++) {
            if (s3 % 11 == 0) continue;
            s1 = (s1 * 31 + v2 + 3) % m;
            for (long v3 = 0; v3 < 2; v3++) {
                if (s3 % 11 == 0) continue;
                A[4 % 7] = (A[4 % 7] + s1 + v3) % m;
                long v4 = 0;
                while (v4 < 2) {
                    v4 += 1;
                    s2 = (s2 + A[(s1 + 5) % 7]) % m;
                    for (long v5 = 0; v5 < 2; v5++) {
                        if (s3 % 11 == 0) continue;
                        if ((long)G.size() < 300) G.push_back((s1 + s2) % 97);
                        for (long v6 = 1; v6 < 4; v6 = v6 * 2) {
                            D[7 % 5] = D[7 % 5] + (s1 % 3);
                            long v7 = 0;
                            while (v7 < 2) {
                                v7 += 1;
                                s3 = (s3 + v7 * v7 + 1) % m;
                                for (long v8 = 0; v8 < 2; v8++) {
                                    if (s3 % 11 == 0) continue;
                                    s1 = (s1 * 31 + v8 + 9) % m;
                                    if (s1 % 4 != 1) {
                                        s3 = (s3 + 9) % m;
                                        A[10 % 7] = (A[10 % 7] + s1 + v8) % m;
                                        run_v10();
                                    } else {
                                        s2 = (s2 + 9) % m;
                                        A[10 % 7] = (A[10 % 7] + s1 + v8) % m;
                                        run_v10();
                                    }
                                    if (s2 % 13 == 0) break;
                                }
                            }
                        }
                        if (s2 % 13 == 0) break;
                    }
                }
                if (s2 % 13 == 0) break;
            }
            if (s2 % 13 == 0) break;
        }
    };

    for (long rep = 0; rep < reps; rep++) {
        if ((long)G.size() < 300) G.push_back((s1 + s2) % 97);
        for (long v0 = 1; v0 < 4; v0 = v0 * 2) {
            D[1 % 5] = D[1 % 5] + (s1 % 3);
            if (s1 % 4 != 1) {
                s3 = (s3 + 1) % m;
                s3 = (s3 + v0 * v0 + 1) % m;
                run_v2();
            } else {
                s2 = (s2 + 1) % m;
                s3 = (s3 + v0 * v0 + 1) % m;
                run_v2();
            }
        }
    }

    long r = 0;
    for (int i = 0; i < 7; i++) r = (r + A[i]) % m;
    for (long x : G) r = (r + x) % m;
    r = (r + (long)G.size()) % m;
    r = (r + s1 + s2 + s3) % m;
    r = (r + D[0] + D[1] + D[2] + D[3] + D[4]) % m;

    printf("result: %ld\n", r);
    return 0;
}

/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/96_find_sum_key.my: find/sum with a key
 * callback over int / double vectors. BENCH-FAIR (as 34_sort_custom_cmp):
 * MyLang's builtin cannot inline its callback, so the key reaches a
 * noinline helper as a std::function and is called once per element. */
#include "bench.h"
#include <functional>
#include <vector>

__attribute__((noinline))
static long sum_key(const std::vector<long> &a,
                    const std::function<long(long)> &key)
{
    long s = 0;
    for (long x : a)
        s += key(x);
    return s;
}

__attribute__((noinline))
static double sum_keyf(const std::vector<double> &a,
                       const std::function<double(double)> &key)
{
    double s = 0;
    for (double x : a)
        s += key(x);
    return s;
}

__attribute__((noinline))
static long find_key(const std::vector<long> &a, long v,
                     const std::function<long(long)> &key)
{
    for (size_t i = 0; i < a.size(); i++)
        if (key(a[i]) == v)
            return (long)i;
    return -1;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    const long n = 500;
    std::vector<long> a;
    std::vector<double> f;
    for (long i = 0; i < n; i++)
        a.push_back((i * 7919) % 1000);
    for (long i = 0; i < n; i++)
        f.push_back(i * 0.5);

    long reps = 1000L * scale;
    long s = 0;
    for (long r = 0; r < reps; r++) {
        s = (s + sum_key(a, [](long x) { return x * x % 97; }))
            % 1000000007;
        s = (s + (long)sum_keyf(f, [](double x) { return x * 3.0; }))
            % 1000000007;
        long k = find_key(a, (r * 13) % 1000,
                          [](long x) { return x + 0; });
        s = (s + (k >= 0 ? k : 7)) % 1000000007;
        bench_sink(s);
    }
    printf("result: %ld\n", s);
    return 0;
}

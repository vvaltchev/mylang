/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/46_matrix_mult.my: naive O(n^3) matmul over
 * arrays-of-arrays (nested subscripting). vector<vector<long>> mirrors the
 * boxed 2-D structure; the LCG fill matches the .my so the checksum agrees. */
#include "bench.h"

typedef std::vector<std::vector<long>> Mat;

static Mat mk(int n, long seed)
{
    Mat m(n);
    long x = seed;
    for (int i = 0; i < n; i++) {
        std::vector<long> row(n);
        for (int j = 0; j < n; j++) {
            x = (x * 1103515245 + 12345) % 1000;
            row[j] = x;
        }
        m[i] = std::move(row);
    }
    return m;
}

static Mat matmul(const Mat &a, const Mat &b, int n)
{
    Mat c(n);
    for (int i = 0; i < n; i++) {
        std::vector<long> row(n);
        for (int j = 0; j < n; j++) {
            long s = 0;
            for (int k = 0; k < n; k++)
                s += a[i][k] * b[k][j];
            row[j] = s;
        }
        /* the checksum only reads c[0][0] + c[n-1][n-1], so without this the
         * compiler DCEs the other O(n^2) elements (computing 2, not the full
         * matrix). MyLang computes the WHOLE matrix - force C++ to as well. */
        bench_sink_ptr(row.data());
        c[i] = std::move(row);
    }
    return c;
}

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    const int n = 70;
    Mat a = mk(n, 1), b = mk(n, 2);

    long checksum = 0;
    for (long r = 0; r < scale; r++) {
        Mat c = matmul(a, b, n);
        checksum += c[0][0] + c[n - 1][n - 1];
    }
    printf("result: %ld\n", checksum);
    return 0;
}

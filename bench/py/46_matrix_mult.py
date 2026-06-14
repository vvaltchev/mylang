# naive O(n^3) matrix multiply over lists-of-lists (nested subscripting)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def mk(n, seed):
    m = [None] * n
    x = seed
    for i in range(n):
        row = [0] * n
        for j in range(n):
            x = (x * 1103515245 + 12345) % 1000
            row[j] = x
        m[i] = row
    return m

def matmul(a, b, n):
    c = [None] * n
    for i in range(n):
        row = [0] * n
        for j in range(n):
            s = 0
            for k in range(n):
                s += a[i][k] * b[k][j]
            row[j] = s
        c[i] = row
    return c

n = 70
a = mk(n, 1)
b = mk(n, 2)

checksum = 0
for r in range(scale):
    c = matmul(a, b, n)
    checksum += c[0][0] + c[n - 1][n - 1]

print("result:", checksum)

# sort with a custom comparison function. Python's sort is key-based by
# default; cmp_to_key is the apples-to-apples mapping for a comparator that is
# invoked on every comparison (like MyLang's compare_func).
import sys
import functools
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

SZ = 50000
R = 2 * scale
checksum = 0

def cmp(p, q):
    if p < q:
        return -1
    if p > q:
        return 1
    return 0

for k in range(R):

    a = [0] * SZ
    x = 987654321

    for i in range(SZ):
        x = (x * 1103515245 + 12345) % 2147483647
        a[i] = x

    a.sort(key=functools.cmp_to_key(cmp))
    checksum += a[0] + a[SZ - 1]

print("result:", checksum)

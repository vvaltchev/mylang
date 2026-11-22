# Twin of bench/my/86_elem_arith_compound.my. All values non-negative, so
# Python's flooring // and % match MyLang's truncating /= and %=.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 400000 * scale
a = [i * 17 + 3 for i in range(64)]

for t in range(N):
    j = t & 63
    a[j] += (t & 1023) + 256
    a[j] -= (t >> 3) & 255
    a[j] *= 3
    a[j] //= 2
    a[j] %= 65536

print("result:", sum(a))

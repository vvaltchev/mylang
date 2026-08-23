# Twin of bench/my/87_elem_shift_compound.my. Values stay non-negative and
# below 2^61 (the &= MASK bound), so MyLang's >>>= equals Python's >>= and
# <<= never wraps.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 400000 * scale
MASK = (1 << 60) - 1
a = [i * 2654435761 + 1 for i in range(64)]

for t in range(N):
    j = t & 63
    a[j] <<= 1
    a[j] |= t & 1
    a[j] &= MASK
    a[j] ^= 40503 * (j + 1)
    a[j] >>= 2
    a[j] >>= 1

print("result:", sum(a))

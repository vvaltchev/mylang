# Twin of bench/my/88_elem_float_compound.my. All values positive, so
# Python's floored float % agrees with MyLang's (and C's fmod).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 400000 * scale
f = [i * 0.75 + 0.5 for i in range(64)]

for t in range(N):
    j = t & 63
    f[j] += 1.25 + (t & 7) * 0.375
    f[j] *= 0.5
    f[j] -= 0.125
    f[j] /= 1.5
    f[j] %= 1.0

s = 0.0
for v in f:
    s += v
print("result:", round(s, 4))

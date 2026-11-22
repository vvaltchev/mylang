# a factory returning one of two closures, called in a loop (see the
# MyLang twin)
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1


def make_op(k):
    if k % 3 == 0:
        base = k
        return lambda x: base + x
    factor = k % 7 + 1
    return lambda x: factor * x


N = 400000 * scale
s = 0
for i in range(N):
    f = make_op(i)
    s = (s + f(i)) % 1000000007
print("result:", s)

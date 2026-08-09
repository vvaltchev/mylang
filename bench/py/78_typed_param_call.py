# indirect calls through closures; the MyLang twin annotates the parameters
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1000000 * scale


def make_adder(base):
    return lambda k: base + k


def make_scaler(f):
    return lambda x: f * x


add = make_adder(7)
scale_it = make_scaler(0.5)

s = 0
t = 0.0

for i in range(N):
    s = s + add(i)
    t = t + scale_it(i)

print("result:", s, t)

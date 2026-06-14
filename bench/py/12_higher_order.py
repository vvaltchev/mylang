# higher-order calls: a function passed as an argument and called indirectly
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def apply(f, x):
    return f(x)

def sq(x):
    return x * x

N = 1000000 * scale
s = 0

for i in range(N):
    s += apply(sq, i)
    s = s % 1000000007

print("result:", s)

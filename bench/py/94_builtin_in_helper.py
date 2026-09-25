# a small helper that calls builtins (abs/min/max), called in a tight loop
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def dist(a, b):
    d = abs(a - b)
    return min(d, 1000) + max(a % 7, b % 5)

N = 1000000 * scale
s = 0
for i in range(N):
    s = (s + dist(i, s % 997)) % 1000000007
print("result:", s)

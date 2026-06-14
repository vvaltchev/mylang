# repeated dictionary lookups by key
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

M = 10000
d = {}
for i in range(M):
    d[i] = i

N = 1000000 * scale
s = 0

for i in range(N):
    s += d[i % M]
    s = s % 1000000007

print("result:", s)

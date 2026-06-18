# preallocated list, random-access write then read
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1000000 * scale
a = [0] * N

for i in range(N):
    a[i] = i * 2

s = 0
for i in range(N):
    s += a[i]

print("result:", s)

# iterate over a dictionary's items. Checksum sums values (order-independent).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

M = 100000
d = {}
for i in range(M):
    d[i] = i * 2

R = 5 * scale
s = 0

for k in range(R):
    for key, val in d.items():
        s += val
    s = s % 1000000007

print("result:", s)

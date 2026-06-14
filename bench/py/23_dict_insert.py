# dictionary insertion (integer keys)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 300000 * scale
d = {}

for i in range(N):
    d[i] = i * 2

print("result:", len(d))

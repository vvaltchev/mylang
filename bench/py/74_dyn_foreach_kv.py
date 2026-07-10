# 2-var foreach over a dict container: for k, v in d.items()
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 200000 * scale

d = {}
for i in range(100):
    d[i] = i * 2

total = 0

for r in range(N):
    for k, v in d.items():
        total += k + v

print("result:", total % 1000000007)

# list concatenation with `+` (builds a fresh list each time)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

a = list(range(500))
b = list(range(500))

N = 100000 * scale
total = 0

for i in range(N):
    c = a + b
    total += len(c)
    total = total % 1000000007

print("result:", total)

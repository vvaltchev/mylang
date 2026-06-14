# sum() builtin over a large list
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

SZ = 1000000
a = list(range(SZ))

R = 5 * scale
total = 0

for k in range(R):
    total += sum(a)
    total = total % 1000000007

print("result:", total)

# in-place list reversal
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

a = list(range(100000))

R = 50 * scale
for k in range(R):
    a.reverse()

print("result:", a[0])

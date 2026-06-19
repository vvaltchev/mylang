# A floating-point reduction (a numeric series): dense double arithmetic.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 2000000 * scale
total = 0.0

for i in range(1, N):
    x = i * 1.0
    total += x / (x + 1.0) - 0.5 / x + 1.0 / (x * x)

print("result:", total)

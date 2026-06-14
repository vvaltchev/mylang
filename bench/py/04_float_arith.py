# floating-point arithmetic loop (IEEE double; MyLang uses long double - see note).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1000000 * scale
x = 1.0

for i in range(N):
    x = x * 1.0000001 + 0.5
    x = x - 0.4999999
    if x > 1000000.0:
        x = x / 3.0

print("result:", round(x, 4))

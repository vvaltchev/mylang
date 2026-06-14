# mixed int/float arithmetic: int->float promotion on every iteration.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1000000 * scale
x = 0.0

for i in range(N):
    x = x + i * 0.5      # int * float -> float
    x = x - i            # float - int -> float
    if x > 1000000.0:
        x = 0.0

print("result:", round(x, 4))

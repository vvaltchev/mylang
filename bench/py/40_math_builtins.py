# numeric builtins in a loop: sqrt/sin/cos/log (double precision - see note).
import sys
import math
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 300000 * scale
s = 0.0

for i in range(1, N):
    x = float(i)
    s += math.sqrt(x) + math.sin(x) + math.cos(x) + math.log(x)

print("result:", round(s, 2))

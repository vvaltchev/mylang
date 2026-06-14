# str()/int() round-trip conversions in a loop
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 300000 * scale
s = 0

for i in range(N):
    str_i = str(i)
    back = int(str_i)
    s += back - i            # always 0

print("result:", s)

# while loop: counter increment + accumulate
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 3000000 * scale
i = 0
s = 0

while i < N:
    s += i
    i += 1

print("result:", s)

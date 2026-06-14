# multiple assignment with tuple unpacking: a, b, c = ...
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 600000 * scale
a = b = c = 0
s = 0

for i in range(N):
    a, b, c = i, i + 1, i + 2
    s += a + b + c
    s = s % 1000000007

print("result:", s)

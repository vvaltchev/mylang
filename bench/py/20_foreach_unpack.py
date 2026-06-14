# for-each with tuple unpacking of [x, y] pairs
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

n = 500000 * scale
pairs = [[i, i * 2] for i in range(n)]

s = 0
for x, y in pairs:
    s += x + y
    s = s % 1000000007

print("result:", s)

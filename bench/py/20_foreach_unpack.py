# for-each with tuple unpacking of [x, y] pairs
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

# Build with an explicit loop (not a comprehension) to match MyLang's loop:
# the element value [i, i*2] is computed, so neither side can use range().
n = 500000 * scale
pairs = [None] * n
for i in range(n):
    pairs[i] = [i, i * 2]

s = 0
for x, y in pairs:
    s += x + y
    s = s % 1000000007

print("result:", s)

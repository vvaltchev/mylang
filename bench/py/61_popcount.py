# Sum of popcounts (Hamming weights) over a range (see the .my for notes).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 200000 * scale
total = 0
for i in range(N):
    x = i
    while x != 0:
        total += x & 1
        x = x >> 1
print("result:", total)

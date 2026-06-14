# deep linear recursion (depth 900). Raise the limit so 900 frames fit.
import sys
sys.setrecursionlimit(10000)
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def sumto(n):
    if n == 0:
        return 0
    return n + sumto(n - 1)

r = 0
for k in range(3000 * scale):
    r = sumto(900)

print("result:", r)

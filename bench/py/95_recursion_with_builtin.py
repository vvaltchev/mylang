# a recursion whose body calls a builtin (max) at every level (depth ~500)
import sys
sys.setrecursionlimit(10000)
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def climb(n, h):
    if n <= 0:
        return h
    return climb(n - 1, max(h, (n * 37) % 1009) - 1)

K = 2000 * scale
s = 0
for k in range(K):
    s = (s + climb(500 + k % 7, k % 11)) % 1000000007
print("result:", s)

# mutual recursion: two functions that call each other (depth ~600), each
# doing a little work per level
import sys
sys.setrecursionlimit(10000)
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def ev(n, acc):
    if n <= 0:
        return acc
    return od(n - 1, (acc * 3 + n) % 1000003)

def od(n, acc):
    if n <= 0:
        return acc + 1
    return ev(n - 1, (acc + n * 7) % 1000003)

K = 2000 * scale
s = 0
for k in range(K):
    s = (s + ev(600 + k % 5, k)) % 1000000007
print("result:", s)

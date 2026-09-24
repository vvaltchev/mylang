# a recursive function that also calls a helper at every level (depth ~500)
import sys
sys.setrecursionlimit(10000)
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def weight(n):
    x = n * 7 + 3
    x = x ^ (x >> 3)
    if x < 0:
        x = 0 - x
    return x % 1009

def walk(n):
    if n <= 0:
        return 0
    return (weight(n) + walk(n - 1)) % 1000000007

K = 2000 * scale
s = 0
for k in range(K):
    s = (s + walk(500 + k % 7)) % 1000000007
print("result:", s)

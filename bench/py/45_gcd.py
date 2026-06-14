# Euclidean GCD computed for many varied operand pairs
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def gcd(a, b):
    while b != 0:
        t = b
        b = a % b
        a = t
    return a

N = 150000 * scale
s = 0

for i in range(1, N):
    s += gcd(i * 12345 % 999983, i * 54321 % 999983 + 1)
    s = s % 1000000007

print("result:", s)

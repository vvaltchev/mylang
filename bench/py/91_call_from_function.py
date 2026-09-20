# call overhead from a FUNCTION: a small helper called in a tight loop that
# lives in a function (drive), not at the top level (08 is the top-level twin)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def mix(a, b):
    x = a * 31 + b
    x = x ^ (x >> 7)
    x = x * 5 + 3
    if x < 0:
        x = 0 - x
    return x % 1000003

def drive(n):
    s = 0
    for i in range(n):
        s = (s + mix(i, s)) % 1000000007
    return s

print("result:", drive(1000000 * scale))

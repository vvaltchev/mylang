# string building by repeated `+=` (CPython's in-place refcount==1 optimization)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 50000 * scale
s = ""

for i in range(N):
    s += str(i)
    s += ","

print("result:", len(s))

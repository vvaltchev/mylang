# cross-frame throw: an exception raised DEPTH frames deep, caught at the top.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

class Err(Exception):
    pass

def deep(n, i):
    if n <= 0:
        raise Err(i)
    deep(n - 1, i)

N = 20000 * scale
DEPTH = 16
caught = 0

for i in range(N):
    try:
        deep(DEPTH, i)
    except Err:
        caught += 1

print("result:", caught)

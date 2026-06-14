# closure with mutable captured state (nonlocal) invoked repeatedly
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def mk(start):
    def inner():
        nonlocal start
        start += 1
        return start
    return inner

c = mk(0)
N = 1000000 * scale
s = 0

for i in range(N):
    s += c()

print("result:", s)

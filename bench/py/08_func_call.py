# function-call overhead: a small function invoked in a tight loop
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def work(a, b):
    return a * b + a - b

N = 1000000 * scale
s = 0

for i in range(N):
    s += work(i, 2)
    s = s % 1000000007

print("result:", s)

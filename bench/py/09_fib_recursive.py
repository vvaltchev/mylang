# recursive fibonacci: tree recursion, ~1.7M calls for fib(29).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

r = 0
for k in range(scale):
    r = fib(29)

print("result:", r)

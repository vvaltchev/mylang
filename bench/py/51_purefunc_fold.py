# Python has no auto-pure / const-folding of user functions, so sq()/cube() are
# actually called every iteration (see the .ml file).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def sq(x):
    return x * x

def cube(x):
    return x * x * x

A = 7
B = 13
C = 5

N = 3000000 * scale
s = 0

for i in range(N):
    k = sq(A) + cube(B) - sq(C) + sq(A + B) * 2
    s += k + i
    s = s % 1000000007

print("result:", s)

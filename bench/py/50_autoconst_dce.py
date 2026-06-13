# Python computes the whole constant guard every iteration (the big arithmetic
# is on the left of `and DEBUG`, so it can't be short-circuited away) and cannot
# remove the branch (see the .ml file).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

A = 7
B = 13
C = 5
D = 3
DEBUG = 0

N = 6000000 * scale
s = 0

for i in range(N):
    if (A * B + C) * (D + A) - B * C + (A - D) * (B + C) > 0 and DEBUG:
        # dead: never executed
        s += i * i + 1
    s += i

print("result:", s)

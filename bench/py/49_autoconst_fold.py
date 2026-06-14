# Python has no const-folding of user variables, so the constant `k`
# sub-expression is recomputed every iteration (see the .my file).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

A = 7
B = 13
C = 5
D = 3

N = 3000000 * scale
s = 0

for i in range(N):
    k = (A * B + C) * (D + A) - B * C + A * B * C * D % 100 + (A - D) * (B + C)
    s += k + i
    s = s % 1000000007

print("result:", s)

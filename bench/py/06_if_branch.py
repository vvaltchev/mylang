# branch-heavy loop: if / elif / else dispatch on every iteration
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1500000 * scale
a = b = c = 0

for i in range(N):
    if i % 3 == 0:
        a += 1
    elif i % 3 == 1:
        b += 1
    else:
        c += 1

print("result:", a + b * 2 + c * 3)

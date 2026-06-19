# Collatz: total stopping time for all starts in 1..N (pure integer arithmetic).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 300000 * scale
total = 0

for i in range(1, N):
    n = i
    steps = 0
    while n != 1:
        if n % 2 == 0:
            n = n // 2          # MyLang int/int is truncating division
        else:
            n = 3 * n + 1
        steps += 1
    total += steps

print("result:", total)

# Repeatedly reduce a list of bools: count the `true`s, many passes.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 200000 * scale
flags = [i % 2 == 0 for i in range(N)]

total = 0
for r in range(50):
    c = 0
    for i in range(N):
        if flags[i]:
            c += 1
    total += c

print("result:", total)

import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

a = list(range(1000))
s = 0
reps = 20000 * scale

for r in range(reps):
    for e in a:
        s = (s + e) % 1000000007

print("result:", s)

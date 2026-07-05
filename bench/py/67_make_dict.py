import sys

scale = 1
if len(sys.argv) > 1:
    scale = int(sys.argv[1])

nkeys = 400
reps = 3000 * scale
ks = list(range(nkeys))

total = 0
for r in range(reps):
    d = {k: k * k + r for k in ks}
    total += d[r % nkeys]

print("result:", total)

import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

a = [1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5]
s = 0.0
hits = 0
reps = 500000 * scale

for r in range(reps):
    for e in a:
        s = (s + e) * 0.5
        if s > 4.0:
            hits += 1

print("result:", "%f" % s, hits)

# indexed unpack: for i, (name, val) in enumerate(rows)
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 100000 * scale

rows = []
for j in range(50):
    rows.append(["item" + str(j), str(j * 2)])

acc = 0
for r in range(N):
    for i, (name, val) in enumerate(rows):
        acc += i + len(name) + len(val)

print("result:", acc % 1000000007)

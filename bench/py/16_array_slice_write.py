# SLICE-THEN-WRITE. Python copies on slice creation; MyLang clones on the
# write. Both pay O(k) per iteration, so times should be comparable.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

base = list(range(1000))

N = 100000 * scale
s = 0

for i in range(N):
    sl = base[0:1000]
    sl[0] = i
    s += sl[0]
    s = s % 1000000007

print("result:", s)

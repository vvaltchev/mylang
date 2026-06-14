# materialize key and value lists (list() to match MyLang's array allocation)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

M = 100000
d = {}
for i in range(M):
    d[i] = i

R = 20 * scale
s = 0

for k in range(R):
    ks = list(d.keys())
    vs = list(d.values())
    s += len(ks) + len(vs)

print("result:", s)

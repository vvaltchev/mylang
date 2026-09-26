# find() and sum() with a KEY callback over int / float lists - one
# callback call per element
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def sum_key(a, key):
    s = 0
    for x in a:
        s += key(x)
    return s

def find_key(a, v, key):
    for i, x in enumerate(a):
        if key(x) == v:
            return i
    return None

n = 500
a = [(i * 7919) % 1000 for i in range(n)]
f = [i * 0.5 for i in range(n)]

reps = 1000 * scale
s = 0
for r in range(reps):
    s = (s + sum_key(a, lambda x: x * x % 97)) % 1000000007
    s = (s + int(sum_key(f, lambda x: x * 3.0))) % 1000000007
    k = find_key(a, (r * 13) % 1000, lambda x: x + 0)
    s = (s + (k if k is not None else 7)) % 1000000007
print("result:", s)

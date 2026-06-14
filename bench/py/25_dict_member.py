# Python has no `d.key` sugar; the equivalent operation is d["key"].
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

d = {"alpha": 1, "beta": 2, "gamma": 3, "delta": 4}

N = 500000 * scale
s = 0

for i in range(N):
    s += d["alpha"] + d["beta"] + d["gamma"] + d["delta"]
    s = s % 1000000007

print("result:", s)

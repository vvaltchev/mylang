# for-each with enumerate (equivalent of MyLang's `indexed`)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

a = list(range(1000000))

R = 3 * scale
s = 0

for k in range(R):
    for i, e in enumerate(a):
        s += i * e
    s = s % 1000000007

print("result:", s)

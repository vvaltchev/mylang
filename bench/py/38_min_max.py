# min()/max() over a large list (same LCG data as MyLang)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

SZ = 1000000
a = [0] * SZ
x = 42
for i in range(SZ):
    x = (x * 1103515245 + 12345) % 2147483647
    a[i] = x

R = 5 * scale
s = 0

for k in range(R):
    s += min(a) + max(a)

print("result:", s)

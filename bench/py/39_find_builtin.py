# find: linear scan in a list (list.index) + substring search (str.find)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

SZ = 20000
a = list(range(SZ))

hay = "abcdefghi." * 1000         # 10000 chars

R = 1000 * scale
s = 0

for k in range(R):
    s += a.index(SZ - 1)          # list find: scans to the last element
    s += hay.find("i.")           # string find: substring search
    s = s % 1000000007

print("result:", s)

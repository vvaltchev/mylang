# list(range(N)) matches MyLang's eager range() allocation (not lazy range).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1000000 * scale
s = 0

for e in list(range(N)):
    s += e

print("result:", s)

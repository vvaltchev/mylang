# map() then filter() with lambdas (per-element function calls, like MyLang)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

SZ = 500000 * scale
a = list(range(SZ))

b = list(map(lambda x: x * 2, a))
c = list(filter(lambda x: x % 3 == 0, b))

print("result:", len(c))

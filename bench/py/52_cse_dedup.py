# Python has neither const-folding nor common-subexpression de-duplication, so
# every iteration re-sorts, re-concatenates and re-sums the constant tables from
# scratch -- twice each (see the .my file, where all of this collapses to two
# integer literals computed once at parse time).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

SZ = 3000
base = list(reversed(range(SZ)))

N = 1500 * scale
s = 0

for i in range(N):
    s = (s + sum(sorted(base)) + sum(sorted(base))
           + sum(base + base) + sum(base + base)) % 1000000007

print("result:", s)

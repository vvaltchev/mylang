# idiomatic fast string building: collect parts in a list, join once
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 200000 * scale
parts = [str(i) for i in range(N)]

joined = ",".join(parts)
print("result:", len(joined))

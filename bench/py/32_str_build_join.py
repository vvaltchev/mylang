# idiomatic fast string building: collect parts in a list, join once
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

# Build the parts with an explicit loop (not a comprehension) to match
# MyLang's loop; the join() is the shared builtin being measured.
N = 200000 * scale
parts = [None] * N
for i in range(N):
    parts[i] = str(i)

joined = ",".join(parts)
print("result:", len(joined))

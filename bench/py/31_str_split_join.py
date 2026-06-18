# split a CSV string into fields and join them back, repeatedly
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

# Build the CSV with an explicit loop (not a generator) to match MyLang's
# setup; the timed work is the split()/join() round-trip below.
parts = [None] * 1000
for i in range(1000):
    parts[i] = str(i)
csv = ",".join(parts)

R = 2000 * scale
total = 0

for k in range(R):
    fields = csv.split(",")
    back = ",".join(fields)
    total += len(fields)

print("result:", total)

# split a CSV string into fields and join them back, repeatedly
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

csv = ",".join(str(i) for i in range(1000))

R = 2000 * scale
total = 0

for k in range(R):
    fields = csv.split(",")
    back = ",".join(fields)
    total += len(fields)

print("result:", total)

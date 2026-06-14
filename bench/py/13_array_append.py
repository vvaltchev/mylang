# grow a list element-by-element via append (amortized O(1))
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1000000 * scale
a = []

for i in range(N):
    a.append(i)

print("result:", len(a))

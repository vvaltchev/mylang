# integer arithmetic mix. `//` matches MyLang's truncating `/` (operands are >= 0).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1000000 * scale
acc = 1

for i in range(1, N):
    acc = (acc + i) * 3
    acc = acc % 1000000007
    acc = acc + i // 2 - i % 7

print("result:", acc)

# nested loops: O(n*m) inner-body evaluation
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

OUT = 1500 * scale
IN = 1000
s = 0

for i in range(OUT):
    for j in range(IN):
        s += i * j
        s = s % 1000000007

print("result:", s)

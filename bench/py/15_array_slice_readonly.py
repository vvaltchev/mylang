# READ-ONLY list slicing in a loop. Python copies the slice eagerly (O(k));
# MyLang takes an O(1) view. Same result, very different cost - see the .ml file.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

base = list(range(1000))

N = 200000 * scale
s = 0

for i in range(N):
    sl = base[1:999]
    s += sl[0] + sl[len(sl) - 1]
    s = s % 1000000007

print("result:", s)

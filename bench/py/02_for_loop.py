# idiomatic counted loop (the natural equivalent of MyLang's C-style for)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 3000000 * scale
s = 0

for i in range(N):
    s += i

print("result:", s)

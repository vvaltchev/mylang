# try/except that NEVER throws — the no-exception hot-path handler overhead.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 500000 * scale
s = 0
caught = 0

for i in range(N):
    try:
        s = (s + i) % 1000000007
    except ZeroDivisionError:
        caught += 1                  # never taken

print("result:", s, caught)

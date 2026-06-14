# READ-ONLY string slicing. Python copies each slice (O(k)); MyLang views it.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

base = "0123456789" * 100      # 1000-char string

N = 200000 * scale
s = 0

for i in range(N):
    sub = base[1:999]
    s += ord(sub[0]) + ord(sub[len(sub) - 1])
    s = s % 1000000007

print("result:", s)

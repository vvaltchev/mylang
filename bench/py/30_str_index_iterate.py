# index into a string char-by-char (s[i] yields a 1-char str)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

s2 = "0123456789" * 500          # 5000 chars

R = 200 * scale
total = 0

for k in range(R):
    for i in range(len(s2)):
        total += ord(s2[i])
    total = total % 1000000007

print("result:", total)

# try/finally on the normal (non-throwing) path — the finally-region cost.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 500000 * scale
s = 0
fin = 0

for i in range(N):
    try:
        s = (s + i) % 1000000007
    finally:
        fin += 1

print("result:", s, fin)

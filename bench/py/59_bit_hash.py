# A 32-bit FNV-style hash mixed over a range (see the .my for notes).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 8000000 * scale
MASK = (1 << 32) - 1
h = 2166136261
for i in range(N):
    h = (h ^ i) & MASK
    h = (h * 16777619) & MASK
    h = ((h << 13) | (h >> 19)) & MASK
print("result:", h)

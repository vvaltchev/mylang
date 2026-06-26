# Bit-packed Sieve of Eratosthenes, 64 bits per word (see the .my for notes).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 400000 * scale
WORDS = (N >> 6) + 1
bits = [0] * WORDS
i = 2
while i * i < N:
    if ((bits[i >> 6] >> (i & 63)) & 1) == 0:
        j = i * i
        while j < N:
            bits[j >> 6] = bits[j >> 6] | (1 << (j & 63))
            j += i
    i += 1
count = 0
for i in range(2, N):
    if ((bits[i >> 6] >> (i & 63)) & 1) == 0:
        count += 1
print("result:", count)

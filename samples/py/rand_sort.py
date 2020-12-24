
import random

# Pre-alloc the array
ar = [ None for x in range(100 * 1000) ]

i = 0
while i < len(ar):
    ar[i] = random.randint(0, 1000 * 1000 * 1000)
    i += 1

ar.sort()
print(ar[:10])

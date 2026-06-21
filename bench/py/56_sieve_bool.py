# Sieve of Eratosthenes over a list of bools: count primes below N.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 2000000 * scale
sieve = [True] * N
sieve[0] = False
sieve[1] = False

i = 2
while i * i < N:
    if sieve[i]:
        j = i * i
        while j < N:
            sieve[j] = False
            j += i
    i += 1

count = 0
for i in range(2, N):
    if sieve[i]:
        count += 1

print("result:", count)

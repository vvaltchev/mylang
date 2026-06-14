# Sieve of Eratosthenes: count primes below n
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def compute_primes(n):

    primes = [None] * n
    for i in range(n):
        primes[i] = True

    primes[0] = False
    primes[1] = False

    i = 2
    while i * i < n:
        if primes[i]:
            j = i * i
            while j < n:
                primes[j] = False
                j += i
        i += 1

    count = 0
    for k in range(2, n):
        if primes[k]:
            count += 1

    return count

n = 1000000 * scale
print("result:", compute_primes(n))

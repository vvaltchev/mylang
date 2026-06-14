# count primes below LIMIT by trial division up to sqrt(n)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def is_prime(n):
    f = 2
    while f * f <= n:
        if n % f == 0:
            return False
        f += 1
    return True

LIMIT = 100000 * scale
count = 0

for n in range(2, LIMIT):
    if is_prime(n):
        count += 1

print("result:", count)

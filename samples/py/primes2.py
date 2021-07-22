# Compute the prime numbers smaller than N
# using the Sieve of Eratosthenes method

import sys

def show_help_and_exit():
    print("Syntax:")
    print("     primes2 <N>")
    sys.exit(0)

def compute_primes(n):

    primes = [ None for i in range(n) ]
    result = []

    primes[0] = False
    primes[1] = False

    for i in range(2, n):
        primes[i] = True

    i = 2
    while i * i < n:

        if primes[i]:

            j = i * i
            while j < n:
                primes[j] = False
                j += i

        i += 1

    for i in range(2, n):
        if primes[i]:
            result.append(i)

    return result


def main():

    try:

        n = int(sys.argv[1])

    except:

        show_help_and_exit()

    print(compute_primes(n))

##### Call main() ######
main()

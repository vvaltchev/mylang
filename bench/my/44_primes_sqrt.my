# count primes below LIMIT by trial division up to sqrt(n)
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

func is_prime(n) {
    for (var f = 2; f * f <= n; f += 1)
        if (n % f == 0)
            return false;
    return true;
}

var LIMIT = 100000 * scale;
var count = 0;

for (var n = 2; n < LIMIT; n += 1)
    if (is_prime(n))
        count += 1;

print("result:", count);

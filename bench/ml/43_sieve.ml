# Sieve of Eratosthenes: count primes below n (array writes + nested while)
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

func compute_primes(n) {

    var primes = array(n);
    for (var i = 0; i < n; i += 1)
        primes[i] = true;

    primes[0] = false;
    primes[1] = false;

    var i = 2;
    while (i * i < n) {
        if (primes[i]) {
            var j = i * i;
            while (j < n) {
                primes[j] = false;
                j += i;
            }
        }
        i += 1;
    }

    var count = 0;
    for (var k = 2; k < n; k += 1)
        if (primes[k])
            count += 1;

    return count;
}

var n = 1000000 * scale;
print("result:", compute_primes(n));

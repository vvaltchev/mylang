# Euclidean GCD computed for many varied operand pairs
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

func gcd(a, b) {
    while (b != 0) {
        var t = b;
        b = a % b;
        a = t;
    }
    return a;
}

var N = 150000 * scale;
var s = 0;

for (var i = 1; i < N; i += 1) {
    s += gcd(i * 12345 % 999983, i * 54321 % 999983 + 1);
    s = s % 1000000007;
}

print("result:", s);

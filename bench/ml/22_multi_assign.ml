# multiple assignment with array expansion: a, b, c = [..]
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var N = 600000 * scale;
var a, b, c = 0;
var s = 0;

for (var i = 0; i < N; i += 1) {
    a, b, c = [i, i + 1, i + 2];
    s += a + b + c;
    s = s % 1000000007;
}

print("result:", s);

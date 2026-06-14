# higher-order calls: a function passed as an argument and called indirectly
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

func apply(f, x) => f(x);
func sq(x) => x * x;

var N = 1000000 * scale;
var s = 0;

for (var i = 0; i < N; i += 1) {
    s += apply(sq, i);
    s = s % 1000000007;
}

print("result:", s);

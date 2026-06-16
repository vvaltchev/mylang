# function-call overhead: a small function invoked in a tight loop
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

func work(a, b) => a * b + a - b;

var N = 1000000 * scale;
var s = 0;

for (var i = 0; i < N; i += 1) {
    s += work(i, 2);
    s = s % 1000000007;
}

print("result:", s);

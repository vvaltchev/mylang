# lambda with a captured, mutable state ([start]) invoked repeatedly
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

func mk(start) => func [start] {
    start += 1;
    return start;
};

var c = mk(0);
var N = 1000000 * scale;
var s = 0;

for (var i = 0; i < N; i += 1)
    s += c();

print("result:", s);

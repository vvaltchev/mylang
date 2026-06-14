# auto-pure + folding: sq()/cube() have no side effects, so MyLang auto-promotes
# them to pure and folds their constant-argument calls to literals. A,B,C are
# write-once (auto-const), so the whole loop-invariant `k` collapses to a single
# literal at "compile" time. Python must actually CALL sq()/cube() every
# iteration (function calls it can't fold). Same result, very different work.
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

func sq(x) => x * x;
func cube(x) => x * x * x;

var A = 7;
var B = 13;
var C = 5;

var N = 3000000 * scale;
var s = 0;

for (var i = 0; i < N; i += 1) {
    var k = sq(A) + cube(B) - sq(C) + sq(A + B) * 2;
    s += k + i;
    s = s % 1000000007;
}

print("result:", s);

# auto-const + dead-code elimination: A..D and DEBUG are write-once, so the loop
# guard is a constant. The big arithmetic is on the left of `&& DEBUG` (DEBUG=0),
# so Python can't short-circuit it away -- it computes the whole guard every
# iteration. MyLang folds the entire guard to a constant false and eliminates the
# branch, leaving just `s += i`. Same result, very different work per iteration.
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var A = 7;
var B = 13;
var C = 5;
var D = 3;
var DEBUG = 0;

var N = 6000000 * scale;
var s = 0;

for (var i = 0; i < N; i += 1) {
    if ((A * B + C) * (D + A) - B * C + (A - D) * (B + C) > 0 && DEBUG) {
        # dead: never executed, and eliminated at compile time
        s += i * i + 1;
    }
    s += i;
}

print("result:", s);

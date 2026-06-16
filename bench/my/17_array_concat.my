# array concatenation with `+` (builds a fresh array each time)
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var a = array(500);
var b = array(500);
for (var i = 0; i < 500; i += 1) {
    a[i] = i;
    b[i] = i;
}

var N = 100000 * scale;
var total = 0;

for (var i = 0; i < N; i += 1) {
    var c = a + b;
    total += len(c);
    total = total % 1000000007;
}

print("result:", total);

# foreach with array-expansion (tuple unpacking) of [x, y] pairs
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var n = 500000 * scale;
var pairs = array(n);
for (var i = 0; i < n; i += 1)
    pairs[i] = [i, i * 2];

var s = 0;
foreach (var x, y in pairs) {
    s += x + y;
    s = s % 1000000007;
}

print("result:", s);

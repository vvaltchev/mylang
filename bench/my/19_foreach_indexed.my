# foreach with the `indexed` keyword (equivalent of Python's enumerate)
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var a = array(1000000);
for (var i = 0; i < len(a); i += 1)
    a[i] = i;

var R = 3 * scale;
var s = 0;

for (var k = 0; k < R; k += 1) {
    foreach (var i, e in indexed a)
        s += i * e;
    s = s % 1000000007;
}

print("result:", s);

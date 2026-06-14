# sum() builtin over a large array
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var SZ = 1000000;
var a = array(SZ);
for (var i = 0; i < SZ; i += 1)
    a[i] = i;

var R = 5 * scale;
var total = 0;

for (var k = 0; k < R; k += 1) {
    total += sum(a);
    total = total % 1000000007;
}

print("result:", total);

# min()/max() over a large array
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var SZ = 1000000;
var a = array(SZ);
var x = 42;
for (var i = 0; i < SZ; i += 1) {
    x = (x * 1103515245 + 12345) % 2147483647;
    a[i] = x;
}

var R = 5 * scale;
var s = 0;

for (var k = 0; k < R; k += 1)
    s += min(a) + max(a);

print("result:", s);

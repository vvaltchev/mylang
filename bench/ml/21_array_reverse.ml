# in-place array reversal via the reverse() builtin
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var a = array(100000);
for (var i = 0; i < len(a); i += 1)
    a[i] = i;

var R = 50 * scale;
for (var k = 0; k < R; k += 1)
    reverse(a);

print("result:", a[0]);

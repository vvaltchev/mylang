# map() then filter() over a large array, each calling a lambda per element
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var SZ = 500000 * scale;
var a = array(SZ);
for (var i = 0; i < SZ; i += 1)
    a[i] = i;

var b = map(func(x) => x * 2, a);
var c = filter(func(x) => x % 3 == 0, b);

print("result:", len(c));

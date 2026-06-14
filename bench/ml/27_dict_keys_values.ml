# keys()/values() builtins: materialize key and value arrays
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var M = 100000;
var d = {};
for (var i = 0; i < M; i += 1)
    d[i] = i;

var R = 20 * scale;
var s = 0;

for (var k = 0; k < R; k += 1) {
    var ks = keys(d);
    var vs = values(d);
    s += len(ks) + len(vs);
}

print("result:", s);

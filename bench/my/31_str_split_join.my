# split() a CSV string into fields and join() them back, repeatedly
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var parts = array(1000);
for (var i = 0; i < 1000; i += 1)
    parts[i] = str(i);
var csv = join(parts, ",");

var R = 2000 * scale;
var total = 0;

for (var k = 0; k < R; k += 1) {
    var fields = split(csv, ",");
    var back = join(fields, ",");
    total += len(fields);
}

print("result:", total);

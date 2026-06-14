# idiomatic fast string building: collect parts in an array, join() once
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var N = 200000 * scale;
var parts = array(N);
for (var i = 0; i < N; i += 1)
    parts[i] = str(i);

var joined = join(parts, ",");
print("result:", len(joined));

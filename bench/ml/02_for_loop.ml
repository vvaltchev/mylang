# classic C-style for loop. Mapped to Python's idiomatic `for i in range(N)`.
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var N = 3000000 * scale;
var s = 0;

for (var i = 0; i < N; i += 1)
    s += i;

print("result:", s);

# branch-heavy loop: if / else if / else dispatch on every iteration
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var N = 1500000 * scale;
var a, b, c = 0;

for (var i = 0; i < N; i += 1) {
    if (i % 3 == 0)
        a += 1;
    else if (i % 3 == 1)
        b += 1;
    else
        c += 1;
}

print("result:", a + b * 2 + c * 3);

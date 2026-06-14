# repeated dictionary lookups by key
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var M = 10000;
var d = {};
for (var i = 0; i < M; i += 1)
    d[i] = i;

var N = 1000000 * scale;
var s = 0;

for (var i = 0; i < N; i += 1) {
    s += d[i % M];
    s = s % 1000000007;
}

print("result:", s);

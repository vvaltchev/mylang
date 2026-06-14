# numeric builtins in a loop: sqrt/sin/cos/log.
# NOTE: MyLang computes these in long double; Python in double. The checksum
# therefore differs slightly in the low digits between the two.
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var N = 300000 * scale;
var s = 0.0;

for (var i = 1; i < N; i += 1) {
    var x = float(i);
    s += sqrt(x) + sin(x) + cos(x) + log(x);
}

print("result:", str(s, 2));

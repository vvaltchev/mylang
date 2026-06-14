# nested loops: O(n*m) inner-body evaluation
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var OUT = 1500 * scale;
var IN = 1000;
var s = 0;

for (var i = 0; i < OUT; i += 1) {
    for (var j = 0; j < IN; j += 1) {
        s += i * j;
        s = s % 1000000007;
    }
}

print("result:", s);

# find(): linear scan in an array + substring search in a string
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var SZ = 20000;
var a = array(SZ);
for (var i = 0; i < SZ; i += 1)
    a[i] = i;

var hay = "";
for (var i = 0; i < 1000; i += 1)
    hay += "abcdefghi.";          # 10000 chars, "z" appears nowhere until appended

var R = 1000 * scale;
var s = 0;

for (var k = 0; k < R; k += 1) {
    s += find(a, SZ - 1);         # array find: scans to the last element
    s += find(hay, "i.");         # string find: substring search
    s = s % 1000000007;
}

print("result:", s);

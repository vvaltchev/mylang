# index into a string char-by-char (s[i] yields a 1-char string)
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var s2 = "";
for (var i = 0; i < 500; i += 1)
    s2 += "0123456789";            # 5000 chars

var R = 200 * scale;
var total = 0;

for (var k = 0; k < R; k += 1) {
    for (var i = 0; i < len(s2); i += 1)
        total += ord(s2[i]);
    total = total % 1000000007;
}

print("result:", total);

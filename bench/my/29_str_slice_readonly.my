# READ-ONLY string slicing in a loop (the string-flavored COW case).
# Strings are immutable in both languages, so the result is identical; only
# the cost differs: MyLang `base[1:999]` is an O(1) view, Python copies O(k).
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var base = "";
for (var i = 0; i < 100; i += 1)
    base += "0123456789";          # 1000-char string

var N = 200000 * scale;
var s = 0;

for (var i = 0; i < N; i += 1) {
    var sub = base[1:999];
    s += ord(sub[0]) + ord(sub[len(sub) - 1]);
    s = s % 1000000007;
}

print("result:", s);

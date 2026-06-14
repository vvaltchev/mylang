# SLICE-THEN-WRITE: the copy-on-write clone actually fires here.
#
# Writing through the slice forces MyLang to clone the shared storage (O(k)),
# which is exactly the up-front cost Python paid when it created the slice.
# So unlike the read-only case, here both languages do O(k) work per iteration
# and the times should be comparable.
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var base = array(1000);
for (var i = 0; i < 1000; i += 1)
    base[i] = i;

var N = 100000 * scale;
var s = 0;

for (var i = 0; i < N; i += 1) {
    var sl = base[0:1000];   # O(1) view ...
    sl[0] = i;               # ... but this write clones the 1000 elements
    s += sl[0];
    s = s % 1000000007;
}

print("result:", s);

# realistic mix: build text, split() into tokens, count word frequencies in a
# dict. Checksum = total token count (order-independent).
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var words = ["the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog"];

var n = 200000 * scale;
var sb = array(n);
var x = 7;
for (var i = 0; i < n; i += 1) {
    x = (x * 1103515245 + 12345) % 8;
    sb[i] = words[x];
}
var text = join(sb, " ");

var counts = {};
var toks = split(text, " ");

foreach (var w in toks) {
    var cur = find(counts, w);
    if (cur == none)
        counts[w] = 1;
    else
        counts[w] = cur + 1;
}

var total = 0;
foreach (var k, v in counts)
    total += v;

print("result:", total, "distinct:", len(counts));

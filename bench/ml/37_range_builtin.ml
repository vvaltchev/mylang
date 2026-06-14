# range() builtin: MyLang range() eagerly builds a whole array (like Python 2).
# The Python version uses list(range(N)) to match that allocation; idiomatic
# Python 3 would use the lazy range object (faster) - see the README note.
var scale = 1;
if (len(argv) > 0)
    scale = int(argv[0]);

var N = 1000000 * scale;
var s = 0;

foreach (var e in range(N))
    s += e;

print("result:", s);

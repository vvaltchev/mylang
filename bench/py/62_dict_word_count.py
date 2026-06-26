# Word-frequency counting with STRING keys (CPython dict).
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

words = ["the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
         "a", "an", "and", "of", "to", "in", "is", "it", "that", "was"]
nw = len(words)
n = 2000000 * scale

counts = {}
for i in range(n):
    w = words[i % nw]
    counts[w] = counts.get(w, 0) + 1

total = 0
for k, v in counts.items():
    total += v * len(k)

print("result:", total)

# realistic mix: build text, split into tokens, count word frequencies in a
# dict. Checksum = total token count (order-independent).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

words = ["the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog"]

n = 200000 * scale
sb = [None] * n
x = 7
for i in range(n):
    x = (x * 1103515245 + 12345) % 8
    sb[i] = words[x]
text = " ".join(sb)

counts = {}
toks = text.split(" ")

for w in toks:
    cur = counts.get(w)
    if cur is None:
        counts[w] = 1
    else:
        counts[w] = cur + 1

total = 0
for k, v in counts.items():
    total += v

print("result:", total, "distinct:", len(counts))

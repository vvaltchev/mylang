# a list of small objects built per iteration: [P(a, b), P(c, d)]
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1


class P:
    __slots__ = ('x', 'y')

    def __init__(self, x, y):
        self.x = x
        self.y = y


N = 500000 * scale
acc = 0

for i in range(N):
    row = [P(i, i + 1), P(i + 2, i + 3)]
    acc += row[0].x + row[1].y
    acc = acc % 1000000007

print("result:", acc)

# build an array of structs, then reduce over their fields
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1


class Point:
    __slots__ = ('x', 'y')

    def __init__(self, x, y):
        self.x = x
        self.y = y


N = 100000 * scale
pts = []

for i in range(N):
    pts.append(Point(i, i * 2))

sx = 0
sy = 0

for p in pts:
    sx += p.x
    sy += p.y

print("result:", sx, sy)

# struct creation: construct structs STANDALONE (not into an array) in a loop,
# read their fields, and accumulate - measures the construction + field-read
# path. Distinct from 58_structs, which builds a list via append; here each
# Point/Vec3 is a fresh standalone value. __slots__ makes CPython instances
# compact, the fairest comparison to MyLang's POD structs.
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1


class Point:
    __slots__ = ("x", "y")

    def __init__(self, x, y):
        self.x = x
        self.y = y


class Vec3:
    __slots__ = ("x", "y", "z")

    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z


N = 500000 * scale
sx = 0
fs = 0.0

for i in range(N):
    p = Point(i, i * 2)                       # standalone int-POD construction
    sx += p.x + p.y
    v = Vec3(i * 1.0, i * 2.0, i * 3.0)       # standalone float-POD construction
    fs += v.x + v.y + v.z

print("result:", sx, int(fs))

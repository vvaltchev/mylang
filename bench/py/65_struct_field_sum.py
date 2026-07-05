# struct field-sum foreach: iterate a flat array<Struct> and sum its scalar
# fields, amplified by an outer rep loop. Exercises the direct struct-field read
# from the array bytes - distinct from 58_structs (mixed) and 64_struct_create
# (construction). __slots__ is the fairest CPython comparison to a POD struct.
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1


class Point3:
    __slots__ = ("x", "y", "z")

    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z


a = [Point3(i, i * 2, i * 3) for i in range(2000)]

s = 0
reps = 2000 * scale
for r in range(reps):
    for p in a:
        s = (s + p.x + p.y + p.z) % 1000000007

print("result:", s)

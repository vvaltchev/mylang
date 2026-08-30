# struct field store: the write twin of 65_struct_field_sum. Two shapes -
# a POD-like struct of scalar fields, and one holding a reference - since
# MyLang lowers them to different tiers (a baked byte store vs. the full
# value lifecycle). __slots__ is the fairest CPython comparison to a POD
# struct, as in 65.
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1


class Pod:
    __slots__ = ("x", "y", "f", "b")

    def __init__(self, x, y, f, b):
        self.x = x
        self.y = y
        self.f = f
        self.b = b


class Boxed:
    __slots__ = ("tag", "n")

    def __init__(self, tag, n):
        self.tag = tag
        self.n = n


def pod_writes(n):
    p = Pod(0, 0, 0.0, False)
    for i in range(n):
        p.x = i
        p.y = i + 1
        p.f = 0.5
        p.b = True
    return p.x + p.y


def boxed_writes(n, tags):
    b = Boxed(None, 0)
    for i in range(n):
        b.tag = tags[i % 3]
        b.n = i
    return b.n


tags = [[1], [2], [3]]
N = 400000 * scale
print("result:", pod_writes(N) + boxed_writes(N, tags))

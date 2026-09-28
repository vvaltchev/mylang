# closures created in the function that calls them (see the MyLang twin)
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1000000 * scale

start = int(0)
base = int(7)


def make():
    st = start

    def tick():
        nonlocal st
        st += 1
        return st
    return tick


tick = make()
add = lambda k: base + k

s = 0
for i in range(N):
    s = s + tick() + add(i)

print("result:", s)

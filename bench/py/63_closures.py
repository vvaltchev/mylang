# closures: create closures in a loop (factories that capture mutable/immutable
# state), call them, and mutate their captures - the whole closure lifecycle
# (creation + capture read/write + indirect call), amplified in a loop.
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1


# a factory whose body captures MUTABLE state (count += 1 in the closure)
def make_counter(start):
    count = start

    def counter():
        nonlocal count
        count += 1
        return count

    return counter


# a factory whose body captures IMMUTABLE state (base is read, not written)
def make_adder(n):
    base = n * 10

    def adder(x):
        return base + x

    return adder


N = 200000 * scale
s = 0

for i in range(N):
    c = make_counter(i)       # create a counter closure
    s += c() + c()            # two capture-mutating calls (i+1, i+2)
    add = make_adder(i)       # create an adder closure
    s += add(i)               # a capture-reading call (11*i)
    s = s % 1000000007

print("result:", s)

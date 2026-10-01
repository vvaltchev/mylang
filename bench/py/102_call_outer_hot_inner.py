# A RUN THAT CALLS, WITH A HOT LOOP THAT DOES NOT: see
# bench/my/102_call_outer_hot_inner.my
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1
M64 = (1 << 64) - 1

def wrap(v):
    v &= M64
    return v - (1 << 64) if v >= (1 << 63) else v

def seed(k):
    x = wrap(k * 2654435761 + 12345)
    x = x ^ (x >> 13)
    if x < 0:
        x = 0 - x
    return x % 65521

def drive(n):
    total = 0
    for o in range(n):
        b = seed(o)
        a0, a1, a2, a3 = b, b + 1, b + 2, b + 3
        a4, a5, a6, a7 = b + 4, b + 5, b + 6, b + 7
        for i in range(100):
            a0 = (a0 * 3 + i) & 65535
            a1 = (a1 ^ (i >> 1)) + 7
            a2 = (a2 + a0) & 65535
            a3 = (a3 * 5 + 1) & 65535
            a4 = (a4 ^ a1) & 65535
            a5 = (a5 + i * 2) & 65535
            a6 = (a6 * 7 + a2) & 65535
            a7 = (a7 + a3 + a5) & 65535
        total = (total + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7) \
            % 1000000007
    return total

print("result:", drive(10000 * scale))

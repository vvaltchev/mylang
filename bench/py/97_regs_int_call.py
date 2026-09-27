# REGISTER PRESSURE ACROSS A CALL: eight int recurrences + one call per
# iteration (see bench/my/97_regs_int_call.my)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def mix(a, b):
    x = a * 31 + b
    x = x ^ (x >> 7)
    x = x * 5 + 3
    if x < 0:
        x = 0 - x
    return x % 1000003

def drive(n):
    a0, a1, a2, a3, a4, a5, a6, a7 = 1, 2, 3, 4, 5, 6, 7, 8
    s = 0
    for i in range(n):
        a0 = (a0 * 3 + i) & 65535
        a1 = (a1 ^ (i >> 1)) + 7
        a2 = (a2 + a0) & 65535
        a3 = (a3 * 5 + 1) & 65535
        a4 = (a4 ^ a1) & 65535
        a5 = (a5 + i * 2) & 65535
        a6 = (a6 * 7 + a2) & 65535
        a7 = (a7 + a3 + a5) & 65535
        s = (s + mix(i, a4)) % 1000000007
    return s + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7

print("result:", drive(500000 * scale))

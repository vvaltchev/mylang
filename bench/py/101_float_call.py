# FLOAT ARGUMENTS ACROSS A CALL: four float recurrences + one call per
# iteration (see bench/my/101_float_call.my)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def blend(a, b):
    t = a * 0.75 + b * 0.25
    t = t + a * b * 0.0001
    if t > 1000.0:
        t = t - 1000.0
    return t * 0.999 + a * 0.0005

def drive(n):
    x0, x1, x2, x3 = 1.0, 2.0, 3.0, 4.0
    s = 0.0
    for i in range(n):
        x0 = x0 * 0.5 + 1.0
        x1 = x1 * 0.25 + x0
        x2 = x2 * 0.125 + x1
        x3 = x3 * 0.5 + x2 * 0.5
        s = blend(s, x3)
    return int(s * 1000.0) + int(x0 + x1 + x2 + x3)

print("result:", drive(500000 * scale))

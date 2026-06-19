# Mandelbrot: sum of escape-iteration counts over a fixed grid (float math).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

SIZE = 200
MAXIT = 80 * scale
total = 0

for py in range(0, SIZE):
    y0 = (py * 1.0 / SIZE) * 2.0 - 1.0       # imaginary part: -1 .. 1
    for px in range(0, SIZE):
        x0 = (px * 1.0 / SIZE) * 3.0 - 2.0   # real part: -2 .. 1
        zr = 0.0
        zi = 0.0
        it = 0
        while it < MAXIT and zr * zr + zi * zi <= 4.0:
            nzr = zr * zr - zi * zi + x0
            zi = 2.0 * zr * zi + y0
            zr = nzr
            it += 1
        total += it

print("result:", total)

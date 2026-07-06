# a native ZeroDivisionError thrown from the runtime, caught in a loop (EAFP).
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

zeros = [0, 0, 0]
N = 200000 * scale
caught = 0

for i in range(N):
    try:
        x = 100 // zeros[i % 3]      # // == MyLang's truncating / ; both throw on 0
        caught += x
    except ZeroDivisionError:
        caught += 1

print("result:", caught)

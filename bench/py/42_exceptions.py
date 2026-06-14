# raise/except in a loop (exceptions as control flow)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

class Even(Exception):
    pass

class Odd(Exception):
    pass

N = 200000 * scale
caught = 0

for i in range(N):
    try:
        if i % 2 == 0:
            raise Even(i)
        else:
            raise Odd(i)
    except Even:
        caught += 1
    except Odd:
        caught += 2

print("result:", caught)

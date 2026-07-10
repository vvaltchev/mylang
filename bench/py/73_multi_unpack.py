# multi-assign destructure of an array value: a, b, c = arr
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 600000 * scale
arr = [0, 0, 0]
s = 0

for i in range(N):
    arr[0] = i
    arr[1] = i + 1
    arr[2] = i + 2
    a, b, c = arr
    s += a + b + c
    s = s % 1000000007

print("result:", s)

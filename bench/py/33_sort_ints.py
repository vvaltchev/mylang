# sort a list of ints (same LCG-generated data as the MyLang version)
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

SZ = 200000
R = 3 * scale
checksum = 0

for k in range(R):

    a = [0] * SZ
    x = 123456789

    for i in range(SZ):
        x = (x * 1103515245 + 12345) % 2147483647
        a[i] = x

    a.sort()
    checksum += a[0] + a[SZ - 1]

print("result:", checksum)

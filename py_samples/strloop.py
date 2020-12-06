#
# Test the performance of str concat
#

s = ""
i = 0

while i < 10000:
    s += str(i)
    s += ","
    i += 1

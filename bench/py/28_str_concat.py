# string building by repeated `+=` (CPython's in-place refcount==1 optimization)
#
# ⛔ THE LOOP LIVES IN A FUNCTION ON PURPOSE. The in-place append fast
# path fires only when the accumulator is a LOCAL (the interpreter needs
# the following STORE_FAST to prove refcount==1); at module level `s` is
# a dict-backed global and every += copies the whole string - O(n^2),
# measured 2 to 11+ MINUTES at scale 25 on WSL2 (page-fault churn) vs
# 0.11s in this form. The module-level spelling silently defeated the
# very optimization this bench exists to measure.
import sys
scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

def main():
    N = 50000 * scale
    s = ""
    for i in range(N):
        s += str(i)
        s += ","
    print("result:", len(s))

main()

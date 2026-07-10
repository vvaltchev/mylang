# discarded call through a func-value picked from a list
import sys

scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1

N = 1000000 * scale


def add_op(st, x):
    st[0] = st[0] + x


def sub_op(st, x):
    st[0] = st[0] - x


ops = [add_op, sub_op]

st = [0]

for i in range(N):
    fn = ops[i % 2]
    fn(st, i)

print("result:", st[0])

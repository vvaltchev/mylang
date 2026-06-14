# Python mirror of verify_semantics.my: every assertion below is identical to
# the MyLang one, demonstrating that the two languages agree on container
# semantics (aliasing, slice independence, clone, by-reference arguments,
# immutable-string slices, shared inner arrays). Run:
#     python3 bench/verify_semantics.py
# Must print "all semantics checks passed", same as the .my version.

# --- plain assignment ALIASES (reference semantics) ------------------------
x = [1, 2, 3]
y = x
y[0] = 99
assert x == [99, 2, 3]
assert y == [99, 2, 3]

y.append(7)
assert x == [99, 2, 3, 7]

# --- a SLICE behaves like an independent copy ------------------------------
a = [1, 2, 3, 4, 5]
s = a[1:4]
assert s == [2, 3, 4]
s[0] = 99
assert a == [1, 2, 3, 4, 5]
assert s == [99, 3, 4]

a2 = [1, 2, 3, 4, 5]
s2 = a2[1:4]
a2[1] = 99
assert a2 == [1, 99, 3, 4, 5]
assert s2 == [2, 3, 4]

f = [1, 2, 3]
g = f[0:3]
g[0] = 99
assert f == [1, 2, 3]
assert g == [99, 2, 3]

# --- explicit copy ---------------------------------------------------------
c1 = [1, 2, 3]
c2 = list(c1)               # MyLang's clone()
c2[0] = 99
assert c1 == [1, 2, 3]
assert c2 == [99, 2, 3]

# --- lists are passed to functions by reference ----------------------------
def mutate(arr):
    arr[0] = 99
    arr.append(7)

p = [1, 2, 3]
mutate(p)
assert p == [99, 2, 3, 7]

# --- strings are immutable; a slice equals the substring -------------------
str1 = "hello world"
sub = str1[0:5]
assert sub == "hello"
assert str1 == "hello world"
assert str1[6:] == "world"
assert str1[-5:] == "world"

# --- nested lists: inner list is shared (reference) ------------------------
m = [[1, 2], [3, 4]]
row = m[0]
row[0] = 99
assert m == [[99, 2], [3, 4]]

print("all semantics checks passed")

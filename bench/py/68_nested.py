import sys
scale = 1
if len(sys.argv) > 1:
    scale = int(sys.argv[1])
A = [1, 2, 3, 4, 5, 6, 7]
G = []
D = {0: 0, 1: 0, 2: 0, 3: 0, 4: 0}
s1 = 1; s2 = 2; s3 = 3
reps = 5000*scale
for rep in range(reps):
    if len(G) < 300: G.append((s1 + s2) % 97)
    v0 = 1
    while v0 < 4:
        D[1 % 5] = D[1 % 5] + (s1 % 3)
        if s1 % 4 != 1:
            s3 = (s3 + 1) % 1000000007
            s3 = (s3 + v0 * v0 + 1) % 1000000007
            for v2 in range(2):
                if s3 % 11 == 0: continue
                s1 = (s1 * 31 + v2 + 3) % 1000000007
                for v3 in range(2):
                    if s3 % 11 == 0: continue
                    A[4 % 7] = (A[4 % 7] + s1 + v3) % 1000000007
                    v4 = 0
                    while v4 < 2:
                        v4 += 1
                        s2 = (s2 + A[(s1 + 5) % 7]) % 1000000007
                        for v5 in range(2):
                            if s3 % 11 == 0: continue
                            if len(G) < 300: G.append((s1 + s2) % 97)
                            v6 = 1
                            while v6 < 4:
                                D[7 % 5] = D[7 % 5] + (s1 % 3)
                                v7 = 0
                                while v7 < 2:
                                    v7 += 1
                                    s3 = (s3 + v7 * v7 + 1) % 1000000007
                                    for v8 in range(2):
                                        if s3 % 11 == 0: continue
                                        s1 = (s1 * 31 + v8 + 9) % 1000000007
                                        if s1 % 4 != 1:
                                            s3 = (s3 + 9) % 1000000007
                                            A[10 % 7] = (A[10 % 7] + s1 + v8) % 1000000007
                                            v10 = 1
                                            while v10 < 4:
                                                s2 = (s2 + A[(s1 + 11) % 7]) % 1000000007
                                                for v11 in range(2):
                                                    if s3 % 11 == 0: continue
                                                    if len(G) < 300: G.append((s1 + s2) % 97)
                                                    v12 = 0
                                                    while v12 < 2:
                                                        v12 += 1
                                                        D[13 % 5] = D[13 % 5] + (s1 % 3)
                                                        v13 = 0
                                                        while v13 < 2:
                                                            v13 += 1
                                                            s3 = (s3 + v13 * v13 + 1) % 1000000007
                                                            for v14 in range(2):
                                                                if s3 % 11 == 0: continue
                                                                s1 = (s1 * 31 + v14 + 15) % 1000000007
                                                                A[v14 % 7] = (A[v14 % 7] * 3 + s1 * 2 - s2 + 15 + 1000000007) % 1000000007
                                                                if s2 % 13 == 0: break
                                                    if s2 % 13 == 0: break
                                                v10 = v10 * 2
                                        else:
                                            s2 = (s2 + 9) % 1000000007
                                            A[10 % 7] = (A[10 % 7] + s1 + v8) % 1000000007
                                            v10 = 1
                                            while v10 < 4:
                                                s2 = (s2 + A[(s1 + 11) % 7]) % 1000000007
                                                for v11 in range(2):
                                                    if s3 % 11 == 0: continue
                                                    if len(G) < 300: G.append((s1 + s2) % 97)
                                                    v12 = 0
                                                    while v12 < 2:
                                                        v12 += 1
                                                        D[13 % 5] = D[13 % 5] + (s1 % 3)
                                                        v13 = 0
                                                        while v13 < 2:
                                                            v13 += 1
                                                            s3 = (s3 + v13 * v13 + 1) % 1000000007
                                                            for v14 in range(2):
                                                                if s3 % 11 == 0: continue
                                                                s1 = (s1 * 31 + v14 + 15) % 1000000007
                                                                A[v14 % 7] = (A[v14 % 7] * 3 + s1 * 2 - s2 + 15 + 1000000007) % 1000000007
                                                                if s2 % 13 == 0: break
                                                    if s2 % 13 == 0: break
                                                v10 = v10 * 2
                                        if s2 % 13 == 0: break
                                v6 = v6 * 2
                            if s2 % 13 == 0: break
                    if s2 % 13 == 0: break
                if s2 % 13 == 0: break
        else:
            s2 = (s2 + 1) % 1000000007
            s3 = (s3 + v0 * v0 + 1) % 1000000007
            for v2 in range(2):
                if s3 % 11 == 0: continue
                s1 = (s1 * 31 + v2 + 3) % 1000000007
                for v3 in range(2):
                    if s3 % 11 == 0: continue
                    A[4 % 7] = (A[4 % 7] + s1 + v3) % 1000000007
                    v4 = 0
                    while v4 < 2:
                        v4 += 1
                        s2 = (s2 + A[(s1 + 5) % 7]) % 1000000007
                        for v5 in range(2):
                            if s3 % 11 == 0: continue
                            if len(G) < 300: G.append((s1 + s2) % 97)
                            v6 = 1
                            while v6 < 4:
                                D[7 % 5] = D[7 % 5] + (s1 % 3)
                                v7 = 0
                                while v7 < 2:
                                    v7 += 1
                                    s3 = (s3 + v7 * v7 + 1) % 1000000007
                                    for v8 in range(2):
                                        if s3 % 11 == 0: continue
                                        s1 = (s1 * 31 + v8 + 9) % 1000000007
                                        if s1 % 4 != 1:
                                            s3 = (s3 + 9) % 1000000007
                                            A[10 % 7] = (A[10 % 7] + s1 + v8) % 1000000007
                                            v10 = 1
                                            while v10 < 4:
                                                s2 = (s2 + A[(s1 + 11) % 7]) % 1000000007
                                                for v11 in range(2):
                                                    if s3 % 11 == 0: continue
                                                    if len(G) < 300: G.append((s1 + s2) % 97)
                                                    v12 = 0
                                                    while v12 < 2:
                                                        v12 += 1
                                                        D[13 % 5] = D[13 % 5] + (s1 % 3)
                                                        v13 = 0
                                                        while v13 < 2:
                                                            v13 += 1
                                                            s3 = (s3 + v13 * v13 + 1) % 1000000007
                                                            for v14 in range(2):
                                                                if s3 % 11 == 0: continue
                                                                s1 = (s1 * 31 + v14 + 15) % 1000000007
                                                                A[v14 % 7] = (A[v14 % 7] * 3 + s1 * 2 - s2 + 15 + 1000000007) % 1000000007
                                                                if s2 % 13 == 0: break
                                                    if s2 % 13 == 0: break
                                                v10 = v10 * 2
                                        else:
                                            s2 = (s2 + 9) % 1000000007
                                            A[10 % 7] = (A[10 % 7] + s1 + v8) % 1000000007
                                            v10 = 1
                                            while v10 < 4:
                                                s2 = (s2 + A[(s1 + 11) % 7]) % 1000000007
                                                for v11 in range(2):
                                                    if s3 % 11 == 0: continue
                                                    if len(G) < 300: G.append((s1 + s2) % 97)
                                                    v12 = 0
                                                    while v12 < 2:
                                                        v12 += 1
                                                        D[13 % 5] = D[13 % 5] + (s1 % 3)
                                                        v13 = 0
                                                        while v13 < 2:
                                                            v13 += 1
                                                            s3 = (s3 + v13 * v13 + 1) % 1000000007
                                                            for v14 in range(2):
                                                                if s3 % 11 == 0: continue
                                                                s1 = (s1 * 31 + v14 + 15) % 1000000007
                                                                A[v14 % 7] = (A[v14 % 7] * 3 + s1 * 2 - s2 + 15 + 1000000007) % 1000000007
                                                                if s2 % 13 == 0: break
                                                    if s2 % 13 == 0: break
                                                v10 = v10 * 2
                                        if s2 % 13 == 0: break
                                v6 = v6 * 2
                            if s2 % 13 == 0: break
                    if s2 % 13 == 0: break
                if s2 % 13 == 0: break
        v0 = v0 * 2
r = 0
for x in A: r = (r + x) % 1000000007
for x in G: r = (r + x) % 1000000007
r = (r + len(G)) % 1000000007
r = (r + s1 + s2 + s3) % 1000000007
r = (r + D[0] + D[1] + D[2] + D[3] + D[4]) % 1000000007
print("result:", r)

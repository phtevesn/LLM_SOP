#!/usr/bin/env python3
"""Exact reference solver (Held-Karp DP over subsets) for small instances (N <= ~16).
usage: brute_force.py file.sop  -> prints optimal cost (or -1 if infeasible)"""
import sys

def load(path):
    txt = open(path).read().split("EDGE_WEIGHT_SECTION")[1].split()
    vals = []
    for t in txt:
        try: vals.append(int(t))
        except ValueError: break
    n = vals[0]; m = vals[1:1 + n * n]
    return n, [m[i * n:(i + 1) * n] for i in range(n)]

def solve(n, T):
    pred = [0] * n                       # direct predecessor masks
    for i in range(n):
        for j in range(n):
            if i != j and T[i][j] < 0:
                pred[i] |= 1 << j
    INF = float("inf")
    dp = {}
    for s in range(n):
        if pred[s] == 0:
            dp[(1 << s, s)] = 0
    for mask in range(1, 1 << n):
        for last in range(n):
            cur = dp.get((mask, last))
            if cur is None: continue
            for nx in range(n):
                if mask >> nx & 1 or (pred[nx] & ~mask): continue
                if T[last][nx] < 0: continue
                key = (mask | 1 << nx, nx)
                v = cur + T[last][nx]
                if v < dp.get(key, INF): dp[key] = v
    full = (1 << n) - 1
    best = min((dp.get((full, l), INF) for l in range(n)), default=INF)
    return -1 if best == INF else best

if __name__ == "__main__":
    n, T = load(sys.argv[1])
    print(solve(n, T))

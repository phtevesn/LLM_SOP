#!/usr/bin/env python3
"""Generate a random SOP instance in the TSPLIB-like .sop format.

usage: gen_instance.py N DENSITY SEED MAXCOST [--multi] > file.sop
  DENSITY: probability that a random pair (i<j in a hidden order) gets a precedence.
  --multi: do not force unique start/end nodes (exercises dummy source/sink handling).
"""
import random, sys

def main():
    n = int(sys.argv[1]); dens = float(sys.argv[2]); seed = int(sys.argv[3]); mc = int(sys.argv[4])
    multi = "--multi" in sys.argv
    rnd = random.Random(seed)
    order = list(range(n)); rnd.shuffle(order)          # hidden topological order
    if not multi:                                         # force node 0 first and node n-1 last
        order.remove(0); order.remove(n - 1); order = [0] + order + [n - 1]
    rank = {v: k for k, v in enumerate(order)}
    T = [[0] * n for _ in range(n)]
    for i in range(n):
        for j in range(n):
            if i != j:
                T[i][j] = rnd.randint(0, mc)
    for a in range(n):
        for b in range(n):
            if rank[a] < rank[b] and rnd.random() < dens:    # a must precede b
                T[b][a] = -1                                   # b depends on a
    if not multi:
        for v in range(1, n):
            T[v][0] = -1
        for v in range(n - 1):
            T[n - 1][v] = -1
        T[0][n - 1] = 10000000
    # a pair cannot be both "i depends on j" and have a positive cost the other way round
    print("NAME: gen%d" % seed); print("TYPE: SOP"); print("DIMENSION: %d" % n)
    print("EDGE_WEIGHT_TYPE: EXPLICIT"); print("EDGE_WEIGHT_FORMAT: FULL_MATRIX")
    print("EDGE_WEIGHT_SECTION"); print(n)
    for row in T:
        print(" ".join(map(str, row)))
    if rnd.random() < 0.5:
        print("EOF")

main()

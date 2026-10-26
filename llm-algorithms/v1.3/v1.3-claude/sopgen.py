"""Random SOP instance generator + two independent reference solvers (brute force, bitmask DP)."""
import itertools, random

def gen(N, rng, dens=0.2, maxc=9, unique_ends=False, transitive_only=False, cyclic=False, asym=True):
    """Return full matrix T (list of lists). T[i][j]<0 : i depends on j (j before i)."""
    order = list(range(N)); rng.shuffle(order)          # hidden topological order
    if unique_ends and N >= 2:                          # node 0 first, node N-1 last (SOPLIB style)
        order = [0] + [x for x in order if x not in (0, N-1)] + [N-1]
    rank = {v: k for k, v in enumerate(order)}
    T = [[rng.randint(0, maxc) for _ in range(N)] for _ in range(N)]
    for i in range(N): T[i][i] = 0
    if not asym:
        for i in range(N):
            for j in range(i): T[i][j] = T[j][i]
    dep = set()
    for a in range(N):
        for b in range(N):
            if rank[a] < rank[b] and rng.random() < dens: dep.add((a, b))      # a before b
    if unique_ends and N >= 2:
        for v in range(1, N): dep.add((0, v))
        for v in range(0, N-1): dep.add((v, N-1))
    for (a, b) in dep: T[b][a] = -1                                            # b depends on a
    if cyclic and N >= 2:
        a, b = rng.sample(range(N), 2); T[a][b] = -1; T[b][a] = -1
    return T

def write(path, T, name="t", eof=True, wrap=0, trailing_blank=True):
    N = len(T)
    with open(path, "w") as f:
        f.write(f"NAME: {name}\nTYPE: SOP\nCOMMENT: test\nDIMENSION: {N}\nEDGE_WEIGHT_TYPE: EXPLICIT\nEDGE_WEIGHT_FORMAT: FULL_MATRIX\nEDGE_WEIGHT_SECTION\n{N}\n")
        flat = [x for r in T for x in r]
        if wrap:
            for k in range(0, len(flat), wrap): f.write(" ".join(map(str, flat[k:k+wrap])) + "\n")
        else:
            for r in T: f.write(" ".join(map(str, r)) + " \n")
        if eof: f.write("EOF\n")
        if trailing_blank: f.write("\n \n")

def feasible_perm(T, p):
    pos = {v: k for k, v in enumerate(p)}
    N = len(T)
    return all(not (i != j and T[i][j] < 0) or pos[j] < pos[i] for i in range(N) for j in range(N))

def brute(T):
    """Independent method 1: enumerate every permutation. Returns optimal cost or None if infeasible."""
    N = len(T); best = None
    if N == 1: return 0
    for p in itertools.permutations(range(N)):
        if not feasible_perm(T, p): continue
        c = sum(T[p[k]][p[k+1]] for k in range(N-1))
        if best is None or c < best: best = c
    return best

def dp(T):
    """Independent method 2: Held-Karp style DP over (subset,last) with precedence on subsets."""
    N = len(T)
    if N == 1: return 0
    need = [0]*N
    for i in range(N):
        for j in range(N):
            if i != j and T[i][j] < 0: need[i] |= 1 << j
    INF = float('inf')
    d = {}
    for v in range(N):
        if need[v] == 0: d[(1 << v, v)] = 0
    for size in range(1, N):
        nd = {}
        for (m, l), c in d.items():
            if bin(m).count('1') != size: continue
            for v in range(N):
                if m >> v & 1 or (need[v] & ~m): continue
                k = (m | 1 << v, v); cc = c + T[l][v]
                if cc < nd.get(k, INF): nd[k] = cc
        d.update(nd)
    full = (1 << N) - 1
    r = [c for (m, l), c in d.items() if m == full]
    return min(r) if r else None

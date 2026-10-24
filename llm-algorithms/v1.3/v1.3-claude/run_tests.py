"""Compare ./sop_solver (or $SOLVER) with independent references on random instances.
usage: run_tests.py [num_instances] [maxN] [extra solver args...]"""
import os, random, subprocess, sys, tempfile, csv, io
sys.path.insert(0, os.path.dirname(__file__))
from sopgen import *
SOLVER = os.environ.get("SOLVER", "./sop_solver")
cnt = int(sys.argv[1]) if len(sys.argv) > 1 else 200
maxN = int(sys.argv[2]) if len(sys.argv) > 2 else 8
extra = sys.argv[3:]
seed = int(os.environ.get("SEED", "7"))
rng = random.Random(seed)
env = dict(os.environ, SOP_DEBUG="1")
fails = 0; opt_yes = 0
tmp = tempfile.mkdtemp()
for it in range(cnt):
    N = rng.randint(1, maxN)
    T = gen(N, rng, dens=rng.choice([0, .05, .15, .3, .6]), maxc=rng.choice([1, 2, 5, 9, 100]),
            unique_ends=rng.random() < .5, cyclic=rng.random() < .04, asym=rng.random() < .8)
    p = os.path.join(tmp, f"i{it}.sop")
    write(p, T, eof=rng.random() < .6, wrap=rng.choice([0, 0, 7, 1000]))
    ref = brute(T) if N <= 8 else dp(T)
    r = subprocess.run([SOLVER, p, "--quiet", "--no-header"] + extra, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        fails += 1; print("CRASH", it, N, r.stderr[-300:]); continue
    row = next(csv.reader(io.StringIO(r.stdout.strip())))
    name, size, tm, nodes, cost, path, optimal = row
    cost = int(cost)
    if ref is None:
        ok = cost == -1 and path == "" and optimal == "no"
    else:
        pl = list(map(int, path.split()))
        ok = (cost == ref and optimal == "yes" and sorted(pl) == list(range(N)) and feasible_perm(T, pl)
              and sum(T[pl[k]][pl[k+1]] for k in range(N-1)) == cost)
        opt_yes += 1
    if not ok:
        fails += 1; print("FAIL it=%d N=%d ref=%s got=%s" % (it, N, ref, row)); os.system(f"cp {p} /tmp/fail_{it}.sop")
print(f"{cnt-fails}/{cnt} passed (feasible+proven: {opt_yes}) args={extra}")
sys.exit(1 if fails else 0)

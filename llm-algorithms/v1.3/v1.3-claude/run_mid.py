"""Mid-size (N 14..24) random instances: solver vs independent C++ DP (tools/dp_ref). args: count threads-args..."""
import os, random, subprocess, sys, tempfile, csv, io
sys.path.insert(0, os.path.dirname(__file__))
from sopgen import *
cnt=int(sys.argv[1]); extra=sys.argv[2:]; rng=random.Random(int(os.environ.get("SEED","11"))); tmp=tempfile.mkdtemp(); fails=0; tot_nodes=0
env=dict(os.environ,SOP_DEBUG="1")
for it in range(cnt):
    N=rng.randint(14,24); T=gen(N,rng,dens=rng.choice([.15,.3,.5]),maxc=rng.choice([9,50,100]),unique_ends=True,asym=rng.random()<.7)
    p=f"{tmp}/m{it}.sop"; write(p,T)
    ref=subprocess.run(["./dp_ref",p,"20000000"],capture_output=True,text=True)
    if ref.returncode!=0: continue
    refc=int(ref.stdout.split()[0])
    r=subprocess.run(["./sop_solver",p,"--quiet","--no-header"]+extra,capture_output=True,text=True,env=env)
    if r.returncode: print("CRASH",it,r.stderr[-200:]); fails+=1; continue
    name,size,tm,nodes,cost,path,opt=next(csv.reader(io.StringIO(r.stdout.strip()))); tot_nodes+=int(nodes)
    pl=list(map(int,path.split()))
    ok=int(cost)==refc and opt=="yes" and feasible_perm(T,pl) and sum(T[pl[k]][pl[k+1]] for k in range(N-1))==refc
    if not ok: fails+=1; print("FAIL",it,N,refc,cost,opt); os.system(f"cp {p} /tmp/midfail_{it}.sop")
print(f"{cnt-fails}/{cnt} ok, total B&B nodes {tot_nodes}, args={extra}")
sys.exit(1 if fails else 0)

#!/usr/bin/env python3
"""Independent subset-DP oracle; interruption and parallel stress checks."""
import csv
import io
from pathlib import Path
import random
import signal
import subprocess
import sys
import tempfile
import time

exe=str(Path(sys.argv[1] if len(sys.argv)>1 else './sop').resolve())
rng=random.Random(82189)

def write(path,t):
    n=len(t)
    path.write_text(f'DIMENSION: {n}\nEDGE_WEIGHT_SECTION\n{n}\n'+'\n'.join(' '.join(map(str,row)) for row in t))

def dp(t):
    n=len(t);pred=[sum(1<<j for j in range(n) if i!=j and t[i][j]<0) for i in range(n)]
    states={(0,-1):0}
    for depth in range(n):
        next_states={}
        for (mask,last),g in states.items():
            for x in range(n):
                if mask>>x&1 or pred[x]&~mask:continue
                key=(mask|1<<x,x);cost=g+(0 if last<0 else t[last][x])
                if cost<next_states.get(key,10**40):next_states[key]=cost
        states=next_states
    return min(states.values())

def row_of(p):
    assert p.returncode==0,(p.returncode,p.stderr)
    rows=list(csv.DictReader(io.StringIO(p.stdout)));assert len(rows)==1
    return rows[0]

def valid(t,r):
    if int(r['Best Cost'])<0:assert not r['Best Path'];return
    path=list(map(int,r['Best Path'].split()));n=len(t)
    assert sorted(path)==list(range(n)),r
    pos={x:k for k,x in enumerate(path)}
    assert all(pos[j]<pos[i] for i in range(n) for j in range(n) if i!=j and t[i][j]<0),r
    assert sum(t[a][b] for a,b in zip(path,path[1:]))==int(r['Best Cost']),r

with tempfile.TemporaryDirectory() as temp:
    p=Path(temp)/'test.sop';comparisons=0
    for trial in range(12):
        n=10+trial%7
        scale=10**12 if trial%3==0 else 1
        t=[[0 if i==j else rng.randrange(50)*scale for j in range(n)] for i in range(n)]
        order=list(range(n));rng.shuffle(order)
        for a in range(n):
            for b in range(a+1,n):
                if rng.random()<[0,.1,.4][trial%3]:t[order[b]][order[a]]=-1
        expected=dp(t);write(p,t)
        for strategy,threads in [('prefix',1),('arcs',1),('portfolio',8)]:
            r=row_of(subprocess.run([exe,str(p),'--strategy',strategy,'--threads',str(threads),'--history-mb','2','--history-mode','shared' if trial%2 else 'local'],capture_output=True,text=True,timeout=60))
            valid(t,r);assert r['Optimal']=='yes' and int(r['Best Cost'])==expected,(expected,r)
            comparisons+=1
    # Wide visited-bitset keys: two chains admit a small independent DP oracle.
    for n in [70,130]:
        t=[[0 if i==j else rng.randrange(1,100) for j in range(n)] for i in range(n)]
        order=list(range(n));rng.shuffle(order)
        for chain in [order[:n//2],order[n//2:]]:
            for a,b in zip(chain,chain[1:]):t[b][a]=-1
        expected=dp(t);write(p,t)
        for threads,mode in [(1,'local'),(4,'shared')]:
            r=row_of(subprocess.run([exe,str(p),'--strategy','prefix','--threads',str(threads),'--history-mb','8','--history-mode',mode],capture_output=True,text=True,timeout=60))
            valid(t,r);assert r['Optimal']=='yes' and int(r['Best Cost'])==expected,(expected,r)
            comparisons+=1
    n=120
    t=[[0 if i==j else rng.randrange(1,1000) for j in range(n)] for i in range(n)]
    write(p,t)
    for strategy in ['prefix','arcs','portfolio']:
        for threads in [1,8,32]:
            r=row_of(subprocess.run([exe,str(p),'--strategy',strategy,'--threads',str(threads),'--time-limit','.05'],capture_output=True,text=True,timeout=15));valid(t,r)
            assert r['Optimal']=='no'
    proc=subprocess.Popen([exe,str(p),'--threads','4','--strategy','prefix'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    time.sleep(.1);proc.send_signal(signal.SIGINT)
    stdout,stderr=proc.communicate(timeout=15)
    assert proc.returncode==0,stderr
    r=list(csv.DictReader(io.StringIO(stdout)))[0];valid(t,r);assert r['Optimal']=='no'
    print(f'PASS: {comparisons} independent subset-DP comparisons (N=10..16 and two-chain N=70,130), 9 timeout runs up to 32 threads, SIGINT.')

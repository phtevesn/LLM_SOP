#!/usr/bin/env python3
"""Independent exhaustive oracle and CLI regression tests (standard library only)."""
import csv
import io
import itertools
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

EXE = str(Path(sys.argv[1] if len(sys.argv)>1 else './sop').resolve())
rng = random.Random(76224)

def write(path, t, eof=True):
    n=len(t)
    # Deliberately wrap independently of rows and use colon-free metadata.
    values=[str(x) for row in t for x in row]
    path.write_text('NAME: test\nCOMMENT: spaces : numbers 12 34\nTYPE: SOP\nDIMENSION '+str(n)+'\nEDGE_WEIGHT_TYPE: EXPLICIT\nEDGE_WEIGHT_FORMAT: FULL_MATRIX\nEDGE_WEIGHT_SECTION\n'+str(n)+'\n'+'\n'.join(' '.join(values[k:k+7]) for k in range(0,len(values),7))+ ('\nEOF\n\n' if eof else ''))

def oracle(t):
    n=len(t);best=None
    predecessors=[(j,i) for i in range(n) for j in range(n) if i!=j and t[i][j]<0]
    for perm in itertools.permutations(range(n)):
        pos=[0]*n
        for k,x in enumerate(perm):pos[x]=k
        if any(pos[j]>=pos[i] for j,i in predecessors):continue
        cost=sum(t[a][b] for a,b in zip(perm,perm[1:]))
        best=cost if best is None else min(best,cost)
    return best

def run(path,*args,okay=True):
    p=subprocess.run([EXE,str(path),*map(str,args)],capture_output=True,text=True,timeout=30)
    if not okay:
        assert p.returncode!=0,(p.stdout,p.stderr)
        return p
    assert p.returncode==0,(p.args,p.stdout,p.stderr)
    rows=list(csv.DictReader(io.StringIO(p.stdout)))
    assert len(rows)==1,p.stdout
    return rows[0]

def validate(t,row,expected=None):
    n=len(t);cost=int(row['Best Cost']);path=list(map(int,row['Best Path'].split()))
    if cost<0:
        assert path==[] and row['Optimal']=='no',row
        return
    assert sorted(path)==list(range(n)),row
    pos={v:i for i,v in enumerate(path)}
    assert all(pos[j]<pos[i] for i in range(n) for j in range(n) if i!=j and t[i][j]<0),row
    assert sum(t[a][b] for a,b in zip(path,path[1:]))==cost,row
    if expected is not None: assert cost==expected,(expected,row)

with tempfile.TemporaryDirectory() as temp:
    root=Path(temp);p=root/'case.sop';count=0
    for n in range(1,9):
        for trial in range(24 if n<8 else 12):
            t=[[0 if i==j else rng.randrange(0,50) for j in range(n)] for i in range(n)]
            order=list(range(n));rng.shuffle(order)
            density=[0,0.1,0.35,0.8][trial%4]
            for a in range(n):
                for b in range(a+1,n):
                    if rng.random()<density:t[order[b]][order[a]]=-rng.randrange(1,9)
            write(p,t,eof=bool(trial%2));expected=oracle(t)
            for strategy,threads in [("prefix",1),("prefix",4),("arcs",1),("arcs",4),("portfolio",4)]:
                row=run(p,'--strategy',strategy,'--threads',threads,'--history-mb',0 if trial%3==0 else 4,'--history-mode','shared' if trial%2 else 'local')
                validate(t,row,expected);assert row['Optimal']=='yes',(t,row)
                count+=1
    t=[[0,1,-1],[-1,0,1],[1,-1,0]];write(p,t)
    row=run(p);assert row['Best Cost']=='-1' and row['Optimal']=='no'
    write(p,[[0,5],[2,0]])
    out=root/'results.csv'
    run(p,'--output',out);run(p,'--output',out)
    assert len(list(csv.DictReader(out.open())))==2
    r=subprocess.run([EXE,str(p),'--no-header','--output',str(out)],capture_output=True,text=True)
    assert r.returncode==0 and len(r.stdout.splitlines())==1
    assert len(out.read_text().splitlines())==4
    r=run(p,'--time-limit',0);assert r['Optimal']=='no' and r['Best Cost']=='-1'
    for content in ['DIMENSION: 2\nEDGE_WEIGHT_SECTION\n3\n0 1 1 0', 'DIMENSION: 2\nEDGE_WEIGHT_SECTION\n2\n0 1 1', 'DIMENSION: 1001\nEDGE_WEIGHT_SECTION\n1001', 'DIMENSION: 2\nEDGE_WEIGHT_SECTION\n2\n0 1 1 0 9']:
        p.write_text(content);run(p,okay=False)
    # Maximum supported dimension, nontrivial index order, unique chain.
    n=1000;order=list(range(n));rng.shuffle(order)
    t=[[0 if i==j else 3 for j in range(n)] for i in range(n)]
    for a,b in zip(order,order[1:]):t[b][a]=-1
    write(p,t,False);row=run(p,'--threads',4);validate(t,row,2997);assert row['Optimal']=='yes'
    # Exercise the actual 1000-node assignment engine with two possible orders.
    t[order[500]][order[499]]=2;t[order[499]][order[500]]=5
    t[order[500]][order[498]]=-1;t[order[501]][order[499]]=-1
    write(p,t,False);row=run(p,'--threads',4);validate(t,row,2996);assert row['Optimal']=='yes'
    # Large integer exactness; no float objective or tolerance.
    t=[[0,10**13,10**13+3],[10**13+1,0,10**13+7],[10**13+2,10**13+4,0]]
    write(p,t);row=run(p);validate(t,row,oracle(t));assert row['Optimal']=='yes'
    print(f'PASS: {count} exhaustive comparisons, 1000-node chain and two-order assignment, large integer costs, cycles, malformed input, timeout and CSV append.')

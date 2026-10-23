#!/usr/bin/env python3
"""Independent exact subset DP oracle, random DAGs, format and CLI regressions."""
import csv
import io
import itertools
from pathlib import Path
import random
import subprocess
import sys
import tempfile

EXE = str(Path(sys.argv[1] if len(sys.argv)>1 else './sop').resolve())
rng = random.Random(19472)
checks = 0

def oracle(a):
    n=len(a)
    pred=[sum(1<<j for j in range(n) if i!=j and a[i][j]<0) for i in range(n)]
    dp={(0,-1):0}
    for depth in range(n):
        nxt={}
        for (mask,last),cost in dp.items():
            for v in range(n):
                if mask>>v&1 or pred[v]&~mask: continue
                key=(mask|1<<v,v)
                c=cost+(a[last][v] if last>=0 else 0)
                nxt[key]=min(nxt.get(key,10**100),c)
        dp=nxt
    return min(dp.values(),default=-1)

def write(p,a,eof=True):
    n=len(a)
    values=[str(x) for row in a for x in row]
    p.write_text('NAME : arbitrary\nTYPE: SOP\nCOMMENT: DIMENSION: not metadata here\nDIMENSION : '+str(n)+'\nEDGE_WEIGHT_TYPE: EXPLICIT\nEDGE_WEIGHT_FORMAT : FULL_MATRIX\nEDGE_WEIGHT_SECTION\n'+str(n)+'\n'+'\n'.join(' \t'.join(values[k:k+7]) for k in range(0,len(values),7))+ ('\nEOF\n' if eof else '\n\n'))

def run(p,*args,valid=True):
    global checks
    r=subprocess.run([EXE,str(p),*map(str,args)],capture_output=True,text=True,timeout=45)
    checks+=1
    if not valid:
        assert r.returncode==2 and not r.stdout,(r.stdout,r.stderr)
        return
    assert r.returncode==0,(r.stdout,r.stderr)
    rows=list(csv.DictReader(io.StringIO(r.stdout)))
    assert len(rows)==1,r.stdout
    return rows[0]

def verify(row,a,expected=None):
    c=int(row['Best Cost']); p=list(map(int,row['Best Path'].split()))
    if c==-1:
        assert not p and row['Optimal']=='no'
    else:
        n=len(a); assert sorted(p)==list(range(n))
        pos={v:k for k,v in enumerate(p)}
        assert all(pos[j]<pos[i] for i in range(n) for j in range(n) if i!=j and a[i][j]<0)
        assert sum(a[u][v] for u,v in zip(p,p[1:]))==c
    if expected is not None:
        assert c==expected,(c,expected,a,row)
        assert row['Optimal']==('yes' if c>=0 else 'no'),row

with tempfile.TemporaryDirectory() as td:
    p=Path(td)/'test, "quoted".sop'
    # Free endpoints, arbitrary endpoint labels, ties, zero costs, asymmetric DAGs.
    for case in range(210):
        n=rng.randint(1,11)
        order=list(range(n)); rng.shuffle(order)
        a=[[0 if i==j else rng.randrange(0,50) for j in range(n)] for i in range(n)]
        density=[0,.08,.25,.6,.95][case%5]
        for j in range(n):
            for i in range(j):
                if rng.random()<density: a[order[j]][order[i]]=-rng.randint(1,8)
        if case%17==0:
            a=[[0 if x>=0 else x for x in row] for row in a]
        write(p,a,case%2==0)
        row=run(p,'--threads',1 if case%2 else 4,'--heuristic-starts',1 if case%3 else 8,'--history-mb',0 if case%7==0 else 1)
        verify(row,a,oracle(a))
        assert row['Instance']==p.name
    # Deliberately force nontrivial enumeration beyond heuristic solution quality.
    for case in range(25):
        n=12
        a=[[0 if i==j else rng.randrange(1000) for j in range(n)] for i in range(n)]
        write(p,a)
        verify(run(p,'--threads',4,'--heuristic-starts',1),a,oracle(a))
    # Exhaustive permutation oracle independent even of subset-DP recurrence.
    for case in range(10):
        n=7; a=[[0 if i==j else rng.randrange(100) for j in range(n)] for i in range(n)]
        a[2][5]=-7; a[0][3]=-2
        expected=min(sum(a[u][v] for u,v in zip(q,q[1:])) for q in itertools.permutations(range(n)) if q.index(5)<q.index(2) and q.index(3)<q.index(0))
        write(p,a); verify(run(p,'--threads',4),a,expected)
    a=[[0,-1],[-1,0]]; write(p,a); verify(run(p),a,-1)
    a=[[0,5],[2,0]]; write(p,a,False)
    out=Path(td)/'out.csv'
    for _ in range(2): verify(run(p,'--output',out),a,2)
    rows=list(csv.reader(io.StringIO(out.read_text())))
    assert len(rows)==3 and rows[0][0]=='Instance'
    r=subprocess.run([EXE,str(p),'--no-header','--output',str(out)],capture_output=True,text=True,check=True)
    assert len(list(csv.reader(io.StringIO(r.stdout))))==1
    assert len(list(csv.reader(io.StringIO(out.read_text()))))==4
    for args in [('--threads',0),('--time-limit','nan'),('--time-limit',-1),('--threads','2x'),('--output',p)]: run(p,*args,valid=False)
    base=p.read_text()
    malformed=[base.replace('DIMENSION : 2','DIMENSION : 3'),base.replace('FULL_MATRIX','UPPER_ROW'),base+'8\n',base.replace('0 \t5 \t2 \t0','0 5 2'),base.replace('0 \t5 \t2 \t0','1 5 2 0'),base.replace('0 \t5 \t2 \t0','0 1.2 2 0')]
    for text in malformed: p.write_text(text); run(p,valid=False)
    # Full supported dimension, forced chain with shuffled endpoints.
    n=1000; order=list(range(n)); rng.shuffle(order)
    a=[[0 if i==j else 1 for j in range(n)] for i in range(n)]
    for u,v in zip(order,order[1:]): a[v][u]=-1
    write(p,a,False); verify(run(p,'--threads',4),a,999)
    row=run(p,'--time-limit','0.000000001','--threads',4)
    assert row['Optimal']=='no' and row['Best Cost']=='-1' and not row['Best Path']
print(f'PASS: {checks} solver runs; random DP/permutation oracles, 1000-node case, parser, CSV, timeout.')

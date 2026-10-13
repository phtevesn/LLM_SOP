#!/usr/bin/env python3
"""Independent exhaustive oracle + parsing/CLI regressions. Python 3.9+."""
import csv
import itertools
import pathlib
import random
import subprocess
import sys
import tempfile

EXE = str(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else './sop_solver').resolve())
rng = random.Random(8127)

def write(path, t, eof=False):
    n=len(t)
    tokens=[str(v) for row in t for v in row]
    # Wrap across row boundaries, with free-text numbers in COMMENT.
    path.write_text(f'NAME : oracle\nCOMMENT: 42 DIMENSION: 999 free text\nDIMENSION : {n}\nEDGE_WEIGHT_FORMAT: FULL_MATRIX\nEDGE_WEIGHT_SECTION\n{n}\n' +
                    '\n'.join(' '.join(tokens[k:k+7]) for k in range(0,len(tokens),7)) + ('\nEOF\n' if eof else '\n\n'))

def oracle(t):
    n=len(t); best=None
    for p in itertools.permutations(range(n)):
        pos={v:k for k,v in enumerate(p)}
        if any(t[i][j]<0 and pos[j]>=pos[i] for i in range(n) for j in range(n) if i!=j): continue
        z=sum(t[a][b] for a,b in zip(p,p[1:]))
        best=z if best is None else min(best,z)
    return best

def run(files, limit='0.2', extra=()):
    return subprocess.run([EXE,'-t',limit,*extra,*map(str,files)],capture_output=True,text=True,check=False)

def check_path(row,t):
    p=list(map(int,row['Best Path'].split())); assert sorted(p)==list(range(len(t)))
    pos={v:k for k,v in enumerate(p)}
    assert all(pos[j]<pos[i] for i in range(len(t)) for j in range(len(t)) if t[i][j]<0)
    assert sum(t[a][b] for a,b in zip(p,p[1:]))==int(row['Best Cost'])

with tempfile.TemporaryDirectory() as tmp:
    d=pathlib.Path(tmp); files=[]; matrices=[]
    for k in range(128):
        n=1+k%8
        t=[[rng.randrange(25) if i!=j else 0 for j in range(n)] for i in range(n)]
        order=list(range(n)); rng.shuffle(order)
        for a in range(n):
            for b in range(a+1,n):
                if rng.random()<0.3: t[order[b]][order[a]]=-rng.randrange(1,5)
        p=d/f'case,{k}.sop';write(p,t,k%2==0);files.append(p);matrices.append(t)
    result=run(files, extra=('--threads','4'))
    assert result.returncode==0,result.stderr
    rows=list(csv.DictReader(result.stdout.splitlines()));assert len(rows)==len(files)
    assert result.stderr.count('OPTIMAL (proven)')==len(files), result.stderr
    for row,t in zip(rows,matrices):
        check_path(row,t);assert int(row['Best Cost'])==oracle(t),(row,t)
    # Independently compare single-worker/no-history and multiworker variants.
    for options in (('--threads','1','--engine','bnb'),('--threads','2','--memory-mb','0','--engine','bnb'),('--threads','4','--engine','dp')):
        result=run(files[:32],extra=options)
        assert result.returncode==0,result.stderr
        for row,t in zip(csv.DictReader(result.stdout.splitlines()),matrices):
            check_path(row,t);assert int(row['Best Cost'])==oracle(t)
    # More than 64 vertices with only one freely reorderable block: a tractable
    # exact oracle still exists, and endpoints are randomized across indices.
    n=70;order=list(range(n));rng.shuffle(order);flex=order[30:37]
    t=[[0 if i==j else rng.randrange(30) for j in range(n)] for i in range(n)]
    rank={v:i for i,v in enumerate(order)}
    for a in range(n):
        for b in range(n):
            if rank[a]<rank[b] and not (a in flex and b in flex):t[b][a]=-1
    expected=min(sum(t[a][b] for a,b in zip(p,p[1:])) for perm in itertools.permutations(flex)
                 for p in [order[:30]+list(perm)+order[37:]])
    wide=d/'wide.sop';write(wide,t)
    result=run([wide],'2',('--threads','4','--engine','bnb'))
    assert result.returncode==0 and 'OPTIMAL (proven)' in result.stderr,result.stderr
    row=next(csv.DictReader(result.stdout.splitlines()));check_path(row,t);assert int(row['Best Cost'])==expected
    # Feasible incumbent even with zero search time, for >64 and >700 nodes.
    for n in (40,75,701):
        t=[[0 if i==j else rng.randrange(1000) for j in range(n)] for i in range(n)]
        order=list(range(n));rng.shuffle(order)
        for a,b in zip(order,order[1:]):t[b][a]=-1
        p=d/'chain.sop';write(p,t)
        result=run([p],'0');assert result.returncode==0,result.stderr
        check_path(next(csv.DictReader(result.stdout.splitlines())),t)
    # Exercise heuristic relocation on larger, partially ordered instances.
    for n in (40,75):
        t=[[0 if i==j else rng.randrange(1000) for j in range(n)] for i in range(n)]
        order=list(range(n));rng.shuffle(order)
        for a in range(n):
            for b in range(a+1,n):
                if rng.random()<0.05:t[order[b]][order[a]]=-1
        p=d/'heuristic.sop';write(p,t)
        result=run([p],'0.2');assert result.returncode==0,result.stderr
        check_path(next(csv.DictReader(result.stdout.splitlines())),t)
    bad=d/'bad.sop'
    for content in ('DIMENSION: 2\nEDGE_WEIGHT_SECTION\n2\n0 1 1',
                    'DIMENSION: 2\nEDGE_WEIGHT_SECTION\n3\n0 1 1 0',
                    'DIMENSION: 2\nEDGE_WEIGHT_SECTION\n2\n0 -1 -1 0',
                    'DIMENSION: 1\nEDGE_WEIGHT_SECTION\n1\n0 extra'):
        bad.write_text(content);result=run([bad,files[0]])
        assert result.returncode==1 and len(list(csv.DictReader(result.stdout.splitlines())))==2
    out=d/'results.csv';result=run([files[0]],extra=('-o',str(out)))
    assert result.returncode==0 and not result.stdout and out.read_text().startswith('Instance,')
    before=files[0].read_bytes();result=run([files[0]],extra=('-o',str(files[0])))
    assert result.returncode==1 and files[0].read_bytes()==before
print('PASS: 128 exhaustive cases; worker/history variants; 70-node exact oracle; large-path, heuristic, parser, and CLI regressions.')

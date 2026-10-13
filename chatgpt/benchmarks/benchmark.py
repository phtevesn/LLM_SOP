#!/usr/bin/env python3
"""Run sequential, independently validated comparisons; retain full CSV paths."""
import argparse
import csv
import hashlib
import io
import json
import pathlib
import platform
import subprocess
import time


def load(path):
    text=path.read_text()
    header,body=text.split('EDGE_WEIGHT_SECTION',1)
    n=None
    for line in header.splitlines():
        fields=line.replace(':',' ',1).split()
        if fields and fields[0]=='DIMENSION':n=int(fields[1])
    tokens=body.split();assert int(tokens[0])==n
    values=list(map(int,tokens[1:1+n*n]));assert len(values)==n*n
    return n,[values[i*n:(i+1)*n] for i in range(n)]


def validate(row,n,t):
    p=list(map(int,row['Best Path'].split()));assert sorted(p)==list(range(n))
    pos={v:k for k,v in enumerate(p)}
    for i in range(n):
        for j in range(n):
            if i!=j and t[i][j]<0:assert pos[j]<pos[i]
    assert all(t[a][b]>=0 for a,b in zip(p,p[1:]))
    assert sum(t[a][b] for a,b in zip(p,p[1:]))==int(row['Best Cost'])


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--baseline-label',default='baseline')
    ap.add_argument('--solver-label',default='v2')
    ap.add_argument('--seed',type=int)
    ap.add_argument('--baseline',required=True,type=pathlib.Path)
    ap.add_argument('--solver',required=True,type=pathlib.Path)
    ap.add_argument('--seconds',type=float,default=5)
    ap.add_argument('--repeats',type=int,default=3)
    ap.add_argument('--threads',type=int,default=4)
    ap.add_argument('--baseline-threads',type=int,
                    help='Pass --threads to the baseline (omit for legacy v1).')
    ap.add_argument('--output',type=pathlib.Path,default=pathlib.Path('benchmark_results'))
    ap.add_argument('inputs',nargs='+',type=pathlib.Path)
    args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    metadata={'platform':platform.platform(),'python':platform.python_version(),
              'seed_override':args.seed,'seconds_per_instance':args.seconds,'repeats':args.repeats,
              'solver_threads':args.threads,'baseline_threads':args.baseline_threads,
              'inputs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in args.inputs},
              'executables':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.baseline,args.solver)}}
    (args.output/'environment.json').write_text(json.dumps(metadata,indent=2)+'\n')
    allrows=[]
    for repetition in range(args.repeats):
        # Alternate order to reduce systematic ordering bias. No simultaneous runs.
        baseline_options=[] if args.baseline_threads is None else ['--threads',str(args.baseline_threads)]
        solvers=[(args.baseline_label,args.baseline,baseline_options),(args.solver_label,args.solver,['--threads',str(args.threads)])]
        if repetition%2:solvers.reverse()
        for path in args.inputs:
            n,t=load(path)
            for name,exe,options in solvers:
                command=[str(exe.resolve()),'-t',str(args.seconds),*options,*(['--seed',str(args.seed)] if args.seed is not None else []),str(path.resolve())]
                begin=time.perf_counter();r=subprocess.run(command,capture_output=True,text=True,check=True)
                wall=time.perf_counter()-begin
                stem=f'{name}_{repetition+1}_{path.stem}'
                (args.output/(stem+'.csv')).write_text(r.stdout)
                (args.output/(stem+'.log')).write_text(r.stderr)
                row=next(csv.DictReader(io.StringIO(r.stdout)));validate(row,n,t)
                allrows.append({'Solver':name,'Repeat':repetition+1,'Instance':path.name,'N':n,
                                'Seconds':row['Time'],'Process Seconds':f'{wall:.6f}',
                                'Nodes':row['Enumerated Nodes'],'Cost':row['Best Cost'],
                                'Proven Optimal':'OPTIMAL (proven)' in r.stderr,'Validated':True})
                print(name,repetition+1,path.name,'cost='+row['Best Cost'],flush=True)
    with (args.output/'summary.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=allrows[0].keys());writer.writeheader();writer.writerows(allrows)

if __name__=='__main__':main()

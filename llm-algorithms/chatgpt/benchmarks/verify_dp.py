#!/usr/bin/env python3
"""Independent exact Python oracle for narrow precedence state spaces.

No Hungarian bound, local pruning, heuristic incumbent, or parallel history.
Memory is capped by the number of distinct states in an individual layer.
"""
import argparse
import pathlib
import time
from benchmark import load

def optimum(path,max_states=100000):
    n,t=load(path)
    predecessors=[sum(1<<j for j in range(n) if i!=j and t[i][j]<0) for i in range(n)]
    frontier={(0,-1):0};expanded=0;peak=1
    for depth in range(n):
        next_layer={}
        for (visited,last),cost in frontier.items():
            expanded+=1
            for v in range(n):
                if visited>>v&1 or predecessors[v]&~visited:continue
                key=(visited|(1<<v),v);value=cost+(t[last][v] if last>=0 else 0)
                if key not in next_layer or value<next_layer[key]:next_layer[key]=value
                if len(next_layer)>max_states:raise RuntimeError('State cap reached: no optimality conclusion')
        if not next_layer:raise RuntimeError('Infeasible precedence constraints')
        frontier=next_layer;peak=max(peak,len(frontier))
    return min(frontier.values()),expanded,peak

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('input',type=pathlib.Path);parser.add_argument('--max-states',type=int,default=100000)
    args=parser.parse_args();begin=time.perf_counter();cost,expanded,peak=optimum(args.input,args.max_states)
    print(f'{args.input.name}: independently proven optimum={cost}; expanded={expanded}; peak_layer_states={peak}; seconds={time.perf_counter()-begin:.6f}')

#!/usr/bin/env python3
"""Reproducible asymmetric SOP instances; synthetic, not SOPLIB/TSPLIB."""
import argparse
import math
import pathlib
import random

def generate(destination):
    destination.mkdir(parents=True,exist_ok=True)
    configurations=[(40,0.02,'random'),(60,0.10,'random'),(100,0.01,'metric'),(160,0.02,'metric'),
                    (200,0.15,'random'),(300,0.30,'random'),(700,0.60,'random'),(200,0.05,'random')]
    for index,(n,density,family) in enumerate(configurations):
        rng=random.Random(6101200+index)
        xy=[(rng.random()*1000,rng.random()*1000) for _ in range(n)]
        t=[[0 if i==j else (rng.randrange(1001) if family=='random' else int(math.hypot(xy[i][0]-xy[j][0],xy[i][1]-xy[j][1]))+rng.randrange(10)) for j in range(n)] for i in range(n)]
        order=list(range(n));rng.shuffle(order)
        for i in range(n):
            for j in range(i+1,n):
                if i==0 or j==n-1 or rng.random()<density:t[order[j]][order[i]]=-1
        path=destination/f'synthetic_{n}_{family}_{density:.2f}.sop'
        path.write_text(f'NAME: {path.stem}\nCOMMENT: synthetic reproducible seed {6101200+index}\nDIMENSION: {n}\nEDGE_WEIGHT_FORMAT: FULL_MATRIX\nEDGE_WEIGHT_SECTION\n{n}\n'+'\n'.join(' '.join(map(str,row)) for row in t)+'\n')
        print(path)
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('destination',type=pathlib.Path);generate(parser.parse_args().destination)

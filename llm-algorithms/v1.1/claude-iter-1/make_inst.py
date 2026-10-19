# Converts the .sop file into inst.txt (N, then N x N matrix) for ils.cpp
import sys
from parse import load

if len(sys.argv) != 2:
    raise SystemExit(f"Usage: {sys.argv[0]} INPUT.sop")

N,T,h=load(sys.argv[1])
with open('inst.txt','w') as f:
    f.write(f"{N}\n")
    for r in T: f.write(' '.join(map(str,r))+'\n')

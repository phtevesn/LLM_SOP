# Converts the .sop file into inst.txt (N, then N x N matrix) for ils.cpp
import sys
from parse import load
N,T,h=load(sys.argv[1] if len(sys.argv)>1 else '/mnt/user-data/uploads/prob_42.sop')
with open('inst.txt','w') as f:
    f.write(f"{N}\n")
    for r in T: f.write(' '.join(map(str,r))+'\n')

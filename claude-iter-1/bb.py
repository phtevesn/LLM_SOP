import numpy as np, time, sys
from scipy.optimize import linear_sum_assignment
from parse import load
N,T,h=load('/mnt/user-data/uploads/prob_42.sop')
pred=[set(j for j in range(N) if j!=i and T[i][j]<0) for i in range(N)]
# transitive closure
anc=[set(p) for p in pred]
ch=True
while ch:
    ch=False
    for i in range(N):
        new=set(anc[i])
        for j in list(anc[i]): new|=anc[j]
        if new!=anc[i]: anc[i]=new; ch=True
S=[i for i in range(N) if not anc[i]]; E=[j for j in range(N) if not any(j in anc[i] for i in range(N))]
print("start",S,"end",E)
s,e=S[0],E[0]
BIG=10**6
C=np.full((N,N),BIG,dtype=np.int64)
for i in range(N):
    for j in range(N):
        if i==j: continue
        if T[i][j]<0: continue
        if j in anc[i]: continue   # j must precede i, arc i->j illegal
        if i in anc[j] or True: pass
        C[i][j]=T[i][j]
# arcs into start / out of end forbidden except closing arc
for i in range(N):
    C[i][s]=BIG; C[e][i]=BIG
C[e][s]=0
# arc i->j illegal also if some node k must be between: i is ancestor of k and k ancestor of j -> i->j adjacent impossible
for i in range(N):
    for j in range(N):
        if i!=j and i in anc[j]:
            if any((i in anc[k]) and (k in anc[j]) for k in range(N)): C[i][j]=BIG
UB=int(sys.argv[1]); nodes=0
best=[UB+1,None]; t0=time.time()
def valid(path):
    pos={v:i for i,v in enumerate(path)}
    return all(pos[j]<pos[i] for i in range(N) for j in anc[i])
def cycles(succ):
    seen=[0]*N;cs=[]
    for i in range(N):
        if not seen[i]:
            c=[];x=i
            while not seen[x]: seen[x]=1;c.append(x);x=succ[x]
            cs.append(c)
    return cs
def solve(Cm):
    global nodes
    nodes+=1
    r,c=linear_sum_assignment(Cm)
    lb=Cm[r,c].sum()
    if lb>=best[0]: return
    succ=[0]*N
    for a,b in zip(r,c): succ[a]=b
    cs=cycles(succ)
    if len(cs)==1:
        path=[s];x=succ[s]
        while x!=s: path.append(x);x=succ[x]
        if path[-1]==e and valid(path):
            best[0]=lb;best[1]=path;print("found",lb,flush=True);return
        # invalid: branch on tour arcs
        arcs=[(a,succ[a]) for a in range(N) if Cm[a][succ[a]]>0 or True]
        cs_arcs=arcs
    else:
        sm=min(cs,key=len)
        cs_arcs=[(a,succ[a]) for a in sm]
    cs_arcs=[(a,b) for a,b in cs_arcs if Cm[a][b]<BIG]
    cs_arcs.sort(key=lambda ab:-Cm[ab[0]][ab[1]])
    # branch: exclude arc k, force arcs before k (by BIG on alternatives)
    Cc=Cm.copy()
    for (a,b) in cs_arcs:
        C2=Cc.copy(); C2[a][b]=BIG
        solve(C2)
        # force arc (a,b) for subsequent branches
        Cc[a,:]=BIG; Cc[:,b]=BIG; Cc[a][b]=Cm[a][b]
solve(C)
print("RESULT best",best[0] if best[1] else f">={UB}", "nodes",nodes,"time",round(time.time()-t0,2))
print(best[1])

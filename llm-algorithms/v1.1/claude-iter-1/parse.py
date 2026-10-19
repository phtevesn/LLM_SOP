import re,sys
def load(fn):
    lines=open(fn).read().split('\n'); hdr={}; i=0
    while i<len(lines):
        l=lines[i].strip()
        if l.startswith('EDGE_WEIGHT_SECTION'): i+=1; break
        if ':' in l:
            k,v=l.split(':',1); hdr[k.strip()]=v.strip()
        i+=1
    N=int(hdr['DIMENSION'])
    toks=[]
    for l in lines[i:]:
        if l.strip().startswith('EOF'): break
        toks+= l.split()
    assert int(toks[0])==N
    v=list(map(int,toks[1:1+N*N])); assert len(v)==N*N
    return N,[v[r*N:(r+1)*N] for r in range(N)],hdr
if __name__=="__main__":
    N,T,h=load(sys.argv[1]); print(N,h)
    pred=[[j for j in range(N) if j!=i and T[i][j]<0] for i in range(N)]
    # check consistency: T[i][j]<0 implies T[j][i]>=0
    print(sum(1 for i in range(N) for j in range(N) if T[i][j]<0 and T[j][i]<0))
    print("no preds:",[i for i in range(N) if not pred[i]])
    succ=[[i for i in range(N) if j in pred[i]] for j in range(N)]
    print("no succs:",[j for j in range(N) if not succ[j]])
    print(sum(len(p) for p in pred))

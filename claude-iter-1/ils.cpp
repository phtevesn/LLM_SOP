#include <bits/stdc++.h>
using namespace std;
int N; int T[64][64]; bool pre[64][64]; // pre[i][j]: j must precede i
vector<int> predl[64];
mt19937_64 rng(12345);
long long cost(const vector<int>&p){long long c=0;for(int i=0;i+1<N;i++)c+=T[p[i]][p[i+1]];return c;}
bool valid(const vector<int>&p){int pos[64];for(int i=0;i<N;i++)pos[p[i]]=i;for(int i=0;i<N;i++)for(int j:predl[i])if(pos[j]>pos[i])return false;return true;}
long long evals=0;
// local search: or-opt (segment len 1..3, both orientations kept), full re-eval w/ validity
void localsearch(vector<int>&p,long long&c){
  bool imp=true;
  while(imp){imp=false;
    for(int L=1;L<=N-2&&!imp;L++){ if(L>3&&L>8)break;
      for(int i=1;i+L<=N-1&&!imp;i++){
        vector<int> seg(p.begin()+i,p.begin()+i+L);
        vector<int> rest(p.begin(),p.begin()+i); rest.insert(rest.end(),p.begin()+i+L,p.end());
        for(int rev=0;rev<2&&!imp;rev++){
          vector<int> s=seg; if(rev){ if(L==1)break; reverse(s.begin(),s.end());}
          for(int k=1;k<(int)rest.size()&&!imp;k++){
            if(k==i&&!rev)continue;
            vector<int> q(rest.begin(),rest.begin()+k); q.insert(q.end(),s.begin(),s.end()); q.insert(q.end(),rest.begin()+k,rest.end());
            evals++;
            long long nc=cost(q);
            if(nc<c&&valid(q)){p=q;c=nc;imp=true;}
          }}}}
  }
}
int main(int argc,char**argv){
  FILE*f=fopen("inst.txt","r");fscanf(f,"%d",&N);for(int i=0;i<N;i++)for(int j=0;j<N;j++)fscanf(f,"%d",&T[i][j]);
  for(int i=0;i<N;i++)for(int j=0;j<N;j++)if(T[i][j]<0)predl[i].push_back(j);
  double tl=atof(argv[1]);
  auto t0=chrono::steady_clock::now();auto el=[&]{return chrono::duration<double>(chrono::steady_clock::now()-t0).count();};
  // start: 0 first, 41 last; random topological order
  auto randinit=[&]{vector<int>p;vector<int>done(N,0);p.push_back(0);done[0]=1;
    while((int)p.size()<N-1){vector<int>c;for(int i=1;i<N-1;i++)if(!done[i]){bool ok=true;for(int j:predl[i])if(!done[j])ok=false;if(ok)c.push_back(i);}
      int x=c[rng()%c.size()];p.push_back(x);done[x]=1;}
    p.push_back(N-1);return p;};
  vector<int>best=randinit();long long bc=cost(best);localsearch(best,bc);
  vector<int>cur=best;long long cc=bc;double tbest=el();int it=0;
  while(el()<tl){
    vector<int>q=cur;
    // perturb: several random feasible moves of a random segment
    int m=2+rng()%4;
    for(int t=0;t<m;t++){
      for(int tries=0;tries<50;tries++){
        int L=1+rng()%4;int i=1+rng()%(N-2-L+1);if(i+L>N-1)continue;
        vector<int> seg(q.begin()+i,q.begin()+i+L);vector<int> rest(q.begin(),q.begin()+i);rest.insert(rest.end(),q.begin()+i+L,q.end());
        int k=1+rng()%(rest.size()-1);vector<int> r(rest.begin(),rest.begin()+k);r.insert(r.end(),seg.begin(),seg.end());r.insert(r.end(),rest.begin()+k,rest.end());
        if(valid(r)){q=r;break;}
      }}
    long long qc=cost(q);localsearch(q,qc);
    if(qc<=cc||(rng()%100<2&&qc<cc+15)){cur=q;cc=qc;}
    if(qc<bc){bc=qc;best=q;tbest=el();}
    if(++it%200==0&&cc>bc+30){cur=best;cc=bc;}
  }
  printf("BEST %lld time_to_best %.3f iters %d evals %lld valid %d\n",bc,tbest,it,evals,(int)valid(best));
  for(int x:best)printf("%d ",x);puts("");
}

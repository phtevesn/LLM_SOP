#include <bits/stdc++.h>
using namespace std;
int N; int start_node=-1, end_node=-1;
vector<vector<long long>> T; // T[i][j]: cost from i to j
vector<vector<int>> predl;   // predl[i]: nodes that must precede i
mt19937_64 rng(12345);
long long cost(const vector<int>&p){long long c=0;for(int i=0;i+1<N;i++)c+=T[p[i]][p[i+1]];return c;}
bool valid(const vector<int>&p){
  if((int)p.size()!=N || p.front()!=start_node || p.back()!=end_node)return false;
  vector<int> pos(N,-1);
  for(int i=0;i<N;i++){
    if(p[i]<0 || p[i]>=N || pos[p[i]]!=-1)return false;
    pos[p[i]]=i;
  }
  for(int i=0;i<N;i++)for(int j:predl[i])if(pos[j]>pos[i])return false;
  return true;
}
long long evals=0;
chrono::steady_clock::time_point deadline;
bool expired(){return chrono::steady_clock::now()>=deadline;}
// local search: or-opt (segment len 1..3, both orientations kept), full re-eval w/ validity
void localsearch(vector<int>&p,long long&c){
  bool imp=true;
  while(imp&&!expired()) {imp=false;
    for(int L=1;L<=N-2&&!imp;L++){ if(L>3&&L>8)break;
      for(int i=1;i+L<=N-1&&!imp;i++){
        if(expired())return;
        vector<int> seg(p.begin()+i,p.begin()+i+L);
        vector<int> rest(p.begin(),p.begin()+i); rest.insert(rest.end(),p.begin()+i+L,p.end());
        for(int rev=0;rev<2&&!imp;rev++){
          vector<int> s=seg; if(rev){ if(L==1)break; reverse(s.begin(),s.end());}
          for(int k=1;k<(int)rest.size()&&!imp;k++){
            if(expired())return;
            if(k==i&&!rev)continue;
            vector<int> q(rest.begin(),rest.begin()+k); q.insert(q.end(),s.begin(),s.end()); q.insert(q.end(),rest.begin()+k,rest.end());
            evals++;
            long long nc=cost(q);
            if(nc<c&&valid(q)){p=q;c=nc;imp=true;}
          }}}}
  }
}
int main(int argc,char**argv){
  if(argc<2){fprintf(stderr,"Usage: %s TIME_LIMIT_SECONDS\n",argv[0]);return 1;}
  double tl=atof(argv[1]);
  if(!isfinite(tl)||tl<=0){fprintf(stderr,"TIME_LIMIT_SECONDS must be positive\n");return 1;}
  ifstream f("inst.txt");
  if(!f){fprintf(stderr,"Cannot open inst.txt\n");return 1;}
  if(!(f>>N)||N<2){fprintf(stderr,"Invalid instance size in inst.txt\n");return 1;}
  T.assign(N,vector<long long>(N));predl.assign(N,{});
  for(int i=0;i<N;i++)for(int j=0;j<N;j++){
    if(!(f>>T[i][j])){fprintf(stderr,"Incomplete instance matrix in inst.txt\n");return 1;}
    if(i!=j&&T[i][j]<0)predl[i].push_back(j);
  }
  vector<int> sources,sinks;vector<vector<int>> succ(N);
  for(int i=0;i<N;i++)for(int j:predl[i])succ[j].push_back(i);
  for(int i=0;i<N;i++){if(predl[i].empty())sources.push_back(i);if(succ[i].empty())sinks.push_back(i);}
  if(sources.empty()||sinks.empty()){fprintf(stderr,"No valid start/end node in inst.txt\n");return 1;}
  start_node=sources.front();end_node=sinks.front();
  auto t0=chrono::steady_clock::now();deadline=t0+chrono::duration_cast<chrono::steady_clock::duration>(chrono::duration<double>(tl));auto el=[&]{return chrono::duration<double>(chrono::steady_clock::now()-t0).count();};
  // Build a random feasible topological path between the derived endpoints.
  auto randinit=[&]{vector<int>p;vector<int>done(N,0);p.push_back(start_node);done[start_node]=1;
    while((int)p.size()<N-1){vector<int>c;for(int i=0;i<N;i++)if(!done[i]&&i!=end_node){bool ok=true;for(int j:predl[i])if(!done[j])ok=false;if(ok)c.push_back(i);}
      if(c.empty())return vector<int>{};
      int x=c[rng()%c.size()];p.push_back(x);done[x]=1;}
    for(int j:predl[end_node])if(!done[j])return vector<int>{};
    p.push_back(end_node);return p;};
  vector<int>best=randinit();
  if(best.empty()||!valid(best)){fprintf(stderr,"Could not construct an initial feasible path\n");return 1;}
  long long bc=cost(best);localsearch(best,bc);
  if(N<=2){printf("BEST %lld time_to_best %.3f iters 0 evals %lld valid %d\n",bc,el(),evals,(int)valid(best));for(int x:best)printf("%d ",x);puts("");return 0;}
  vector<int>cur=best;long long cc=bc;double tbest=el();int it=0;
  while(el()<tl&&!expired()){
    vector<int>q=cur;
    // perturb: several random feasible moves of a random segment
    int m=2+rng()%4;
    for(int t=0;t<m&&!expired();t++){
      for(int tries=0;tries<50;tries++){
        int maxL=min(4,N-2);int L=1+rng()%maxL;int i=1+rng()%(N-2-L+1);if(i+L>N-1)continue;
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

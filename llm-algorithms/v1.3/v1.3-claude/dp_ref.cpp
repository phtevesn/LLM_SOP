// Independent exact reference: layered DP over (scheduled-set, last). N<=64. No bounds, no heuristics.
// Prints optimal cost (or gives up if state count exceeds the cap).
#include <bits/stdc++.h>
using namespace std; typedef unsigned long long u64; typedef long long ll;
struct H{size_t operator()(const pair<u64,int>&p)const{return p.first*0x9e3779b97f4a7c15ULL^(size_t)p.second*0xbf58476d1ce4e5b9ULL;}};
int main(int argc,char**argv){
  ifstream f(argv[1]); string line; vector<ll> v; bool sec=false; 
  while(getline(f,line)){ if(!sec){ if(line.find("EDGE_WEIGHT_SECTION")!=string::npos) sec=true; continue;}
    stringstream ss(line); string t; while(ss>>t){ if(t=="EOF")goto done; v.push_back(atoll(t.c_str()));}}
  done:; int N=v[0]; vector<ll> T(v.begin()+1,v.begin()+1+N*N);
  size_t cap = argc>2? atoll(argv[2]) : 60000000;
  vector<u64> need(N,0); for(int i=0;i<N;i++)for(int j=0;j<N;j++) if(i!=j&&T[i*N+j]<0) need[i]|=1ULL<<j;
  unordered_map<pair<u64,int>,ll,H> cur,nxt; 
  for(int s=0;s<N;s++) if(!need[s]) cur[{1ULL<<s,s}]=0;
  size_t peak=0;
  for(int layer=1;layer<N;layer++){ nxt.clear(); nxt.reserve(cur.size()*2);
    for(auto&kv:cur){ u64 m=kv.first.first; int l=kv.first.second; ll c=kv.second;
      for(int w=0;w<N;w++){ if(m>>w&1||(need[w]&~m)) continue; ll cc=c+T[l*N+w]; auto key=make_pair(m|1ULL<<w,w);
        auto it=nxt.find(key); if(it==nxt.end()) nxt.emplace(key,cc); else if(cc<it->second) it->second=cc; } }
    peak=max(peak,nxt.size()); if(nxt.size()>cap){ printf("GIVEUP layer %d states %zu\n",layer,nxt.size()); return 3;}
    swap(cur,nxt);}
  ll best=LLONG_MAX; for(auto&kv:cur) best=min(best,kv.second);
  printf("%lld (peak layer states %zu)\n",best,peak); }

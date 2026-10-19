// Independent exhaustive verification of the dynamic assignment relaxation.
#define SOP_LIBRARY
#include "sop_solver.cpp"
#include <cassert>

Cost assignment_oracle(const Problem& g,const Assignment& a) {
    std::vector<int> rows,cols;
    for(int i=0;i<g.n;i++){if(a.active_row[i])rows.push_back(i);if(a.active_col[i])cols.push_back(i);}
    Cost best=INF;
    do {
        Cost value=0;bool feasible=true;
        for(size_t i=0;i<rows.size();i++) {
            if(!g.arc[rows[i]][cols[i]]){feasible=false;break;}
            value+=g.c[rows[i]][cols[i]];
        }
        if(feasible)best=std::min(best,value);
    }while(std::next_permutation(cols.begin(),cols.end()));
    return best;
}
void verify(const Problem& g,const Assignment& a) {
    assert(a.value==assignment_oracle(g,a));
    Cost dual=0;
    for(int i=0;i<g.n;i++) {
        if(a.active_row[i]){dual+=a.u[i];assert(a.row[i]>=0 && a.active_col[a.row[i]] && a.col[a.row[i]]==i);}
        if(a.active_col[i])dual+=a.v[i];
        for(int j=0;j<g.n;j++)if(a.active_row[i] && a.active_col[j] && g.arc[i][j])assert(a.u[i]+a.v[j]<=g.c[i][j]);
    }
    assert(dual==a.value);
}
void prefixes(Shared& sh,const Assignment& ap,std::vector<int>& path,std::vector<int>& degree,std::vector<char>& used,int& checked) {
    verify(sh.g,ap);++checked;
    if(int(path.size())==sh.g.n)return;
    int last=path.back();
    for(int b=0;b<sh.g.n;b++)if(!used[b] && degree[b]==0) {
        Assignment child=ap;
        Cost reduced=sh.g.c[last][b]-ap.u[last]-ap.v[b];
        bool ok=child.fix(last,b,sh);assert(ok);
        assert(sh.g.c[last][b]+child.value>=ap.value+reduced);
        used[b]=1;path.push_back(b);for(int w:sh.g.succ[b])--degree[w];
        prefixes(sh,child,path,degree,used,checked);
        for(int w:sh.g.succ[b])++degree[w];
        path.pop_back();used[b]=0;
    }
}
int main(){
    std::mt19937 rng(78234);int checked=0;
    for(int trial=0;trial<100;trial++) {
        int n=1+trial%6;Input in{n,std::vector<std::vector<Cost>>(n,std::vector<Cost>(n))};
        std::vector<int> order(n);std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),rng);
        for(int a=0;a<n;a++)for(int b=0;b<n;b++)if(a!=b)in.t[a][b]=rng()%30;
        for(int a=0;a<n;a++)for(int b=a+1;b<n;b++)if(rng()%3==0)in.t[order[b]][order[a]]=-1;
        Problem g(in);Shared sh(g,Clock::now()+std::chrono::seconds(60));Assignment ap(g.n);assert(ap.initialize(sh));
        std::vector<int> path{g.s},degree=g.indegree;std::vector<char> used(g.n);used[g.s]=1;for(int w:g.succ[g.s])--degree[w];
        prefixes(sh,ap,path,degree,used,checked);
    }
    std::cout<<"PASS: "<<checked<<" assignment states checked against exhaustive matching, dual feasibility, and reduced-cost bounds.\n";
}

// Independent Held-Karp oracle and multithreaded proof/bound stress checks.
#define SOP_LIBRARY
#include "sop_solver.cpp"
#include <cassert>
Cost oracle(const Input& in) {
    int n=in.n,states=1<<n;std::vector<unsigned> pred(n);
    for(int a=0;a<n;a++)for(int b=0;b<n;b++)if(in.t[a][b]<0)pred[a]|=1U<<b;
    std::vector<Cost> dp(size_t(states)*n,INF);
    for(int a=0;a<n;a++)if(!pred[a])dp[(size_t(1)<<a)*n+a]=0;
    for(unsigned set=1;set<unsigned(states);set++)for(int a=0;a<n;a++) {
        Cost c=dp[size_t(set)*n+a];if(c==INF)continue;
        for(int b=0;b<n;b++)if(!(set&(1U<<b)) && !(pred[b]&~set)) {
            auto& target=dp[size_t(set|(1U<<b))*n+b];target=std::min(target,c+in.t[a][b]);
        }
    }
    Cost best=INF;for(int a=0;a<n;a++)best=std::min(best,dp[size_t(states-1)*n+a]);return best;
}
int main() {
    std::mt19937 rng(28912);int checked=0,proven=0;std::uint64_t donated=0;
    for(int trial=0;trial<32;trial++) {
        int n=8+trial%5;Input in{n,std::vector<std::vector<Cost>>(n,std::vector<Cost>(n))};
        std::vector<int> order(n);std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),rng);
        for(int a=0;a<n;a++)for(int b=0;b<n;b++)if(a!=b)in.t[a][b]=1+rng()%100;
        for(int a=0;a<n;a++)for(int b=a+1;b<n;b++)if(rng()%100<unsigned(trial%4)*10)in.t[order[b]][order[a]]=-1;
        Cost best=oracle(in);
        for(int mode=0;mode<4;mode++) {
            double seconds=mode==3?0.003:0.35;
            int workers=mode==0?1:mode==1?4:8;
            auto r=solve(in,Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)),seconds,workers,
                         trial%3?16ULL*1024*1024:0,trial+123,mode==2?"dp":"bnb");
            assert(r.lower<=best && r.cost>=best && r.lower<=r.cost);
            if(r.optimal){assert(r.cost==best);++proven;}
            donated+=r.donations;++checked;
            Problem g(in);std::vector<int> full{g.s};full.insert(full.end(),r.path.begin(),r.path.end());full.push_back(g.t);assert(g.validate(full)==r.cost);
        }
    }
    // Three isolated cheap clusters: optimum is 3*5 + 2*50 = 115.
    // Assignment relaxations retain cheap cycles, deliberately creating hard
    // interrupted searches whose reported frontier bound must remain sound.
    int unfinished=0;std::uint64_t interrupted_nodes=0;
    Input clustered{18,std::vector<std::vector<Cost>>(18,std::vector<Cost>(18))};
    for(int a=0;a<18;a++)for(int b=0;b<18;b++)clustered.t[a][b]=a==b?0:a/6==b/6?1:50;
    for(int workers:{1,4,8}) {
        auto r=solve(clustered,Clock::now()+std::chrono::milliseconds(20),0.02,workers,16ULL*1024*1024,99,"bnb");
        assert(r.lower<=115 && r.cost>=115);if(r.optimal)assert(r.cost==115);else ++unfinished;
        if(!r.optimal)interrupted_nodes+=r.nodes;
        donated+=r.donations;++checked;
    }
    assert(unfinished>0);
    // Do not let heuristic warmup silently turn this into a zero-search test.
    assert(interrupted_nodes>0);
    assert(proven>=64);assert(donated>0);
    SharedHistory history(4*1024*1024,2);std::vector<std::thread> workers;
    for(int w=0;w<8;w++)workers.emplace_back([&,w](){for(int i=1000;i>=0;i--)history.test("same exact state",i+w);});
    for(auto& worker:workers)worker.join();
    assert(history.test("same exact state",0));assert(!history.test("same exact state",0,true));assert(history.test("same exact state",1,true));assert(!history.test("different state",1,true));
    std::cout<<"PASS: "<<checked<<" engine/oracle comparisons, "<<proven<<" proofs, "<<donated<<" donated tasks; shared-history ownership and interrupted lower bounds checked.\n";
}

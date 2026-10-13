// Standalone SOP solver. C++17, standard library only.
#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <optional>
#include <cstring>
#include <mutex>
#include <thread>
#include <exception>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
using Cost = std::int64_t;
using Clock = std::chrono::steady_clock;
constexpr Cost INF = std::numeric_limits<Cost>::max()/4;

std::string trim(std::string s) {
    auto a=s.find_first_not_of(" \t\r\n");
    return a==std::string::npos ? "" : s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
Cost integer(const std::string& s) {
    size_t k=0; Cost v;
    try { v=std::stoll(s,&k); } catch (...) { throw std::runtime_error("Invalid integer: "+s); }
    if(k!=s.size()) throw std::runtime_error("Invalid integer: "+s);
    return v;
}
struct Input { int n; std::vector<std::vector<Cost>> t; };
Input read_sop(const std::string& file) {
    std::ifstream f(file); if(!f) throw std::runtime_error("Cannot open input");
    std::string line; int n=0; bool section=false;
    while(std::getline(f,line)) {
        line=trim(line); if(line.empty()) continue;
        auto p=line.find_first_of(": \t");
        std::string key=line.substr(0,p), value=p==std::string::npos ? "" : trim(line.substr(p));
        if(!value.empty() && value[0]==':') value=trim(value.substr(1));
        if(key=="EDGE_WEIGHT_SECTION") {
            if(!value.empty()) throw std::runtime_error("EDGE_WEIGHT_SECTION must be on its own line");
            section=true; break;
        }
        if(key=="DIMENSION") {
            Cost d=integer(value);
            if(d<1 || d>10000) throw std::runtime_error("DIMENSION must be 1..10000");
            if(n && n!=d) throw std::runtime_error("Conflicting DIMENSION headers");
            n=static_cast<int>(d);
        } else if(key=="EDGE_WEIGHT_FORMAT" && value!="FULL_MATRIX") {
            throw std::runtime_error("Only FULL_MATRIX is supported");
        }
        // NAME, TYPE, EDGE_WEIGHT_TYPE and free-text COMMENT are metadata.
    }
    if(!section || !n) throw std::runtime_error("Missing DIMENSION or EDGE_WEIGHT_SECTION");
    std::string token;
    if(!(f>>token) || integer(token)!=n) throw std::runtime_error("Repeated dimension does not match");
    Input in{n,std::vector<std::vector<Cost>>(n,std::vector<Cost>(n))};
    for(int i=0;i<n;i++) for(int j=0;j<n;j++) {
        if(!(f>>token)) throw std::runtime_error("Truncated matrix");
        Cost v=integer(token);
        if(i==j && v!=0) throw std::runtime_error("Diagonal must be zero");
        if(v>=0 && v>INF/(n+2)) throw std::runtime_error("Costs too large for safe 64-bit sums");
        in.t[i][j]=v;
    }
    // Read exactly N*N entries; accept only whitespace and optional EOF after them.
    if(f>>token) {
        if(token!="EOF") throw std::runtime_error("Unexpected data after matrix");
        if(f>>token) throw std::runtime_error("Unexpected data after EOF");
    }
    return in;
}
// Immutable problem data shared by all workers. Internal endpoints cost zero.
struct Problem {
    const Input& input;
    int n, s, t, words;
    std::vector<std::vector<Cost>> c;
    std::vector<std::vector<std::uint64_t>> after, before;
    std::vector<std::vector<int>> succ;
    std::vector<int> indegree;
    std::vector<std::vector<unsigned char>> arc;
    bool precedes(int a,int b) const {return (after[a][b/64]>>(b%64))&1;}
    explicit Problem(const Input& in):input(in),n(in.n+2),s(in.n),t(in.n+1),words((n+63)/64),
      c(n,std::vector<Cost>(n)),after(n,std::vector<std::uint64_t>(words)),
      before(n,std::vector<std::uint64_t>(words)),succ(n),indegree(n),arc(n,std::vector<unsigned char>(n)) {
        auto edge=[&](int a,int b){succ[a].push_back(b);++indegree[b];after[a][b/64]|=std::uint64_t(1)<<(b%64);};
        for(int a=0;a<in.n;a++) for(int b=0;b<in.n;b++) {
            c[a][b]=in.t[a][b];if(a!=b && c[a][b]<0) edge(b,a);
        }
        for(int v=0;v<in.n;v++){edge(s,v);edge(v,t);}
        for(int k=0;k<n;k++) for(int a=0;a<n;a++) if(precedes(a,k))
            for(int w=0;w<words;w++) after[a][w]|=after[k][w];
        for(int a=0;a<n;a++) {
            if(precedes(a,a)) throw std::runtime_error("Cyclic precedence constraints: infeasible");
            for(int b=0;b<n;b++) if(precedes(a,b)) before[b][a/64]|=std::uint64_t(1)<<(a%64);
        }
        for(int a=0;a<n;a++) for(int b=0;b<n;b++) {
            if(a==b || a==t || b==s || c[a][b]<0 || precedes(b,a)) continue;
            bool between=false;
            // An obligatory intermediate vertex makes adjacency impossible.
            for(int w=0;w<words;w++) if(after[a][w]&before[b][w]) {between=true;break;}
            arc[a][b]=!between;
        }
    }
    Cost cost(const std::vector<int>& p) const {
        Cost z=0;for(size_t i=1;i<p.size();i++)z+=c[p[i-1]][p[i]];return z;
    }
    Cost validate(const std::vector<int>& full) const {
        if(int(full.size())!=n || full.front()!=s || full.back()!=t)
            throw std::runtime_error("Validation: endpoints or length");
        std::vector<int> pos(input.n,-1);
        for(int i=1;i<n-1;i++) {
            int v=full[i];if(v<0 || v>=input.n || pos[v]>=0)throw std::runtime_error("Validation: permutation");pos[v]=i;
        }
        for(int a=0;a<input.n;a++) for(int b=0;b<input.n;b++)
            if(a!=b && input.t[a][b]<0 && pos[b]>=pos[a])throw std::runtime_error("Validation: precedence");
        Cost sum=0;
        for(int i=2;i<n-1;i++) {
            Cost z=input.t[full[i-1]][full[i]];if(z<0)throw std::runtime_error("Validation: negative edge");sum+=z;
        }
        return sum;
    }
};
// Exact full-key shared history. Locks serialize ownership of equal states;
// a lower-cost arrival can supersede an in-progress, more expensive prefix.
// Equal-cost arrivals never mutually cancel: only the first owns expansion.
class SharedHistory {
    struct Shard {std::mutex lock;std::unordered_map<std::string,Cost> entries;};
    std::array<Shard,64> shards;
    size_t capacity;
public:
    SharedHistory(size_t budget,int words):capacity(budget/(64*(160+size_t(words)*8))){}
    bool test(const std::string& key,Cost cost,bool strict=false) {
        if(!capacity)return false;
        auto& shard=shards[std::hash<std::string>{}(key)%shards.size()];
        std::lock_guard<std::mutex> guard(shard.lock);
        auto found=shard.entries.find(key);
        if(found!=shard.entries.end()) {
            if(strict)return found->second<cost;
            if(found->second<=cost)return true;
            found->second=cost;return false;
        }
        if(strict)return false;
        if(shard.entries.size()>=capacity)shard.entries.clear();
        shard.entries.emplace(key,cost);return false;
    }
};

struct Shared {
    const Problem& g;
    Clock::time_point deadline;
    std::atomic<Cost> best{INF};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> nodes{0}, dominated{0}, bound_prunes{0}, repairs{0};
    SharedHistory history;
    std::atomic<std::uint64_t> local_prunes{0},donations{0};
    std::mutex lock;
    std::vector<int> path;
    Shared(const Problem& p,Clock::time_point end,size_t history_budget=0):g(p),deadline(end),history(history_budget,p.words){}
    bool expired() const {return stop.load(std::memory_order_relaxed)||Clock::now()>=deadline;}
    void offer(const std::vector<int>& p) {
        Cost z=g.cost(p);if(z>=best.load(std::memory_order_relaxed))return;
        // Validate every published improvement, not just the final result.
        if(g.validate(p)!=z)throw std::runtime_error("Validation: incumbent cost");
        std::lock_guard<std::mutex> guard(lock);
        if(z<best.load(std::memory_order_relaxed)){path=p;best.store(z,std::memory_order_relaxed);}
    }
    std::vector<int> snapshot(){std::lock_guard<std::mutex> guard(lock);return path;}
};

// A primal/dual optimal assignment for the active rows and columns.
// Fixing last->next deletes one row/column; at most ONE augmentation repairs it.
struct Assignment {
    std::vector<int> row, col;
    std::vector<Cost> u,v;
    std::vector<unsigned char> active_row,active_col;
    Cost value=0;
    explicit Assignment(int n):row(n,-1),col(n,-1),u(n),v(n),active_row(n,1),active_col(n,1){}
    // Returns false on interruption OR infeasibility. Caller checks deadline.
    bool augment(int root,Shared& sh) {
        const auto& g=sh.g;int n=g.n;
        std::vector<Cost> dist(n,INF);std::vector<int> from(n,-1);
        std::vector<unsigned char> seen(n);int a=root,previous=-1;
        while(true) {
            if(sh.expired())return false;
            Cost delta=INF;int target=-1;
            for(int b=0;b<n;b++) if(active_col[b] && !seen[b]) {
                if(g.arc[a][b]) {
                    Cost reduced=g.c[a][b]-u[a]-v[b];
                    if(reduced<dist[b]){dist[b]=reduced;from[b]=previous;}
                }
                if(dist[b]<delta){delta=dist[b];target=b;}
            }
            if(target<0 || delta==INF)return false;
            u[root]+=delta;
            for(int b=0;b<n;b++) if(active_col[b]) {
                if(seen[b]) {u[col[b]]+=delta;v[b]-=delta;}
                else if(dist[b]!=INF)dist[b]-=delta;
            }
            if(col[target]<0) {
                int b=target;
                while(b>=0) {
                    int prev=from[b];int r=prev<0?root:col[prev];
                    row[r]=b;col[b]=r;b=prev;
                }
                return true;
            }
            seen[target]=1;previous=target;a=col[target];
        }
    }
    bool initialize(Shared& sh) {
        active_row[sh.g.t]=0;active_col[sh.g.s]=0;
        for(int a=0;a<sh.g.n;a++) if(active_row[a]) {
            if(!augment(a,sh))return false;
        }
        recost(sh.g);return true;
    }
    void recost(const Problem& g){value=0;for(int a=0;a<g.n;a++)if(active_row[a])value+=g.c[a][row[a]];}
    bool fix(int a,int b,Shared& sh) {
        int old=row[a], displaced=col[b];
        active_row[a]=0;active_col[b]=0;row[a]=-1;col[b]=-1;
        if(old!=b) {
            col[old]=-1;row[displaced]=-1;
            ++sh.repairs;
            if(!augment(displaced,sh))return false;
        }
        recost(sh.g);return true;
    }
};

class Heuristic {
    Shared& sh;const Problem& g;std::mt19937_64 rng;
    std::vector<double> pheromone;unsigned iteration=0;
    bool done(Clock::time_point until)const{return sh.expired()||Clock::now()>=until;}
    // Transitive precedence rectangle prefix sums make a block-swap test O(1).
    std::vector<int> rectangles(const std::vector<int>& p)const {
        int z=g.n+1;std::vector<int> pref(z*z);
        for(int i=0;i<g.n;i++)for(int j=0;j<g.n;j++)
            pref[(i+1)*z+j+1]=int(g.precedes(p[i],p[j]))+pref[i*z+j+1]+pref[(i+1)*z+j]-pref[i*z+j];
        return pref;
    }
    bool exchange(std::vector<int>& p,Clock::time_point until,bool full) {
        int n=g.n,z=n+1;auto pref=rectangles(p);
        auto independent=[&](int i,int j,int k){
            return pref[j*z+k]-pref[i*z+k]-pref[j*z+j]+pref[i*z+j]==0;
        };
        // Swap adjacent non-reversed blocks [i,j), [j,k). This is a directed
        // precedence-safe 3-edge exchange. For large N limit the shorter block.
        for(int j=2;j<n-1;j++) {
            if(done(until))return false;
            for(int i=j-1;i>=1;i--)for(int k=j+1;k<n;k++) {
                if(!full && j-i>3 && k-j>3)break;
                if(!independent(i,j,k))continue;
                int a=p[i-1],b=p[i],c=p[j-1],d=p[j],e=p[k-1],f=p[k];
                Cost delta=g.c[a][d]+g.c[e][b]+g.c[c][f]-g.c[a][b]-g.c[c][d]-g.c[e][f];
                if(delta<0){std::rotate(p.begin()+i,p.begin()+j,p.begin()+k);return true;}
            }
        }
        return false;
    }
    bool relocate(std::vector<int>& p,Clock::time_point until) {
        bool changed=false;
        for(int i=1;i<g.n-1;i++) {
            if(done(until))break;
            int x=p[i],a=p[i-1],b=p[i+1];
            Cost removal=g.c[a][b]-g.c[a][x]-g.c[x][b],delta=0;int where=-1;
            for(int j=i-2;j>=0;j--) {
                if(g.precedes(p[j+1],x))break;
                Cost d=removal+g.c[p[j]][x]+g.c[x][p[j+1]]-g.c[p[j]][p[j+1]];
                if(d<delta){delta=d;where=j;}
            }
            for(int j=i+1;j<g.n-1;j++) {
                if(g.precedes(x,p[j]))break;
                Cost d=removal+g.c[p[j]][x]+g.c[x][p[j+1]]-g.c[p[j]][p[j+1]];
                if(d<delta){delta=d;where=j;}
            }
            if(where>=0){p.erase(p.begin()+i);p.insert(p.begin()+(where<i?where+1:where),x);changed=true;}
        }
        return changed;
    }
    void ruin_recreate(std::vector<int>& p) {
        int count=std::min(g.n-2,3+int(rng()%16));std::vector<int> removed;
        for(int q=0;q<count;q++) {int at=1+int(rng()%unsigned(p.size()-2));removed.push_back(p[at]);p.erase(p.begin()+at);}
        std::shuffle(removed.begin(),removed.end(),rng);
        for(int x:removed) {
            int low=1,high=int(p.size())-1;
            for(int i=0;i<int(p.size());i++) {
                if(g.precedes(p[i],x))low=std::max(low,i+1);
                if(g.precedes(x,p[i]))high=std::min(high,i);
            }
            if(low>high)throw std::runtime_error("Internal repair: invalid insertion interval");
            Cost best=INF;int at=low;
            for(int i=low;i<=high;i++) {
                Cost delta=g.c[p[i-1]][x]+g.c[x][p[i]]-g.c[p[i-1]][p[i]];
                if(delta<best){best=delta;at=i;}
            }
            p.insert(p.begin()+at,x);
        }
    }
    // Exact optimization of a contiguous window, with fixed outside endpoints.
    // Outside precedence remains valid because no vertex leaves the window.
    bool window(std::vector<int>& p,int lo,int k,Clock::time_point until) {
        int states=1<<k;std::vector<unsigned> need(k);
        for(int a=0;a<k;a++)for(int b=0;b<k;b++)if(g.precedes(p[lo+b],p[lo+a]))need[a]|=1U<<b;
        std::vector<Cost> dp(size_t(states)*k,INF);
        std::vector<signed char> parent(size_t(states)*k,-1);
        for(int a=0;a<k;a++)if(!need[a])dp[(size_t(1)<<a)*k+a]=g.c[p[lo-1]][p[lo+a]];
        for(unsigned mask=1;mask<unsigned(states);mask++) {
            if((mask&255)==0 && done(until))return false;
            unsigned available=0;
            for(int b=0;b<k;b++)if(!(mask&(1U<<b)) && !(need[b]&~mask))available|=1U<<b;
            for(int a=0;a<k;a++)if(dp[size_t(mask)*k+a]!=INF) {
                for(int b=0;b<k;b++)if(available&(1U<<b)) {
                    size_t dest=size_t(mask|(1U<<b))*k+b;
                    Cost candidate=dp[size_t(mask)*k+a]+g.c[p[lo+a]][p[lo+b]];
                    if(candidate<dp[dest]){dp[dest]=candidate;parent[dest]=static_cast<signed char>(a);}
                }
            }
        }
        Cost best=INF;int last=-1;
        for(int a=0;a<k;a++)if(dp[size_t(states-1)*k+a]!=INF) {
            Cost candidate=dp[size_t(states-1)*k+a]+g.c[p[lo+a]][p[lo+k]];
            if(candidate<best){best=candidate;last=a;}
        }
        Cost old=0;for(int a=lo;a<=lo+k;a++)old+=g.c[p[a-1]][p[a]];
        if(best>=old)return false;
        std::vector<int> replacement(k);unsigned mask=unsigned(states-1);
        for(int a=k-1;a>=0;a--){replacement[a]=p[lo+last];int previous=parent[size_t(mask)*k+last];mask^=1U<<last;last=previous;}
        std::copy(replacement.begin(),replacement.end(),p.begin()+lo);return true;
    }
public:
    Heuristic(Shared& s,std::uint64_t seed):sh(s),g(s.g),rng(seed){}
    std::vector<int> greedy(bool random) {
        auto degree=g.indegree;std::vector<int> ready{g.s},p;
        while(!ready.empty()) {
            int last=p.empty()?g.s:p.back();
            std::sort(ready.begin(),ready.end(),[&](int a,int b){
                return g.c[last][a]!=g.c[last][b]?g.c[last][a]<g.c[last][b]:a<b;
            });
            size_t index=random?size_t(rng()%std::min<size_t>(ready.size(),4)):0;
            int v=ready[index];ready.erase(ready.begin()+static_cast<std::ptrdiff_t>(index));p.push_back(v);
            for(int w:g.succ[v])if(--degree[w]==0)ready.push_back(w);
        }
        if(int(p.size())!=g.n)throw std::runtime_error("Cyclic precedence constraints");
        return p;
    }
    // ACS-inspired construction: bounded pheromone, exploitation/roulette
    // choice, local evaporation, and reinforcement of the validated incumbent.
    // This is a heuristic component, never an optimality certificate.
    std::vector<int> colony() {
        if(pheromone.empty())pheromone.assign(size_t(g.n)*g.n,1.0);
        auto elite=sh.snapshot();
        for(double& tau:pheromone)tau=std::max(0.02,tau*0.95);
        for(size_t i=1;i<elite.size();i++)pheromone[size_t(elite[i-1])*g.n+elite[i]]+=0.05*g.n;
        auto degree=g.indegree;std::vector<int> ready{g.s},path;
        std::uniform_real_distribution<double> unit(0.0,1.0);
        while(!ready.empty()) {
            int last=path.empty()?g.s:path.back();std::vector<double> weight(ready.size());
            double sum=0,best=-1;size_t chosen=0;
            for(size_t i=0;i<ready.size();i++) {
                double distance=1.0+double(g.c[last][ready[i]]);
                weight[i]=pheromone[size_t(last)*g.n+ready[i]]/(distance*distance);sum+=weight[i];
                if(weight[i]>best){best=weight[i];chosen=i;}
            }
            if(unit(rng)>0.85) {
                double target=unit(rng)*sum;
                for(size_t i=0;i<ready.size();i++){target-=weight[i];if(target<=0){chosen=i;break;}}
            }
            int v=ready[chosen];ready.erase(ready.begin()+static_cast<std::ptrdiff_t>(chosen));
            auto& tau=pheromone[size_t(last)*g.n+v];tau=0.9*tau+0.1;
            path.push_back(v);for(int w:g.succ[v])if(--degree[w]==0)ready.push_back(w);
        }
        return path;
    }
    void run(Clock::time_point until) {
        auto p=sh.snapshot();
        while(!done(until)) {
            while(!done(until) && relocate(p,until))sh.offer(p);
            sh.offer(p);if(done(until))break;
            // Cheap restarts are interleaved with stronger neighborhoods, so
            // expensive inference never consumes the entire improvement budget.
            if(iteration%3==0) {
                while(!done(until) && exchange(p,until,g.n<=122)) {
                    sh.offer(p);while(!done(until) && relocate(p,until))sh.offer(p);
                }
                sh.offer(p);
            }
            if(done(until))break;
            if(iteration%8==0) {
                int k=std::min(14,g.n-2);int lo=1+int(rng()%unsigned(g.n-k-1));
                if(window(p,lo,k,until)){sh.offer(p);++iteration;continue;}
            }
            ++iteration;
            if(iteration%3==0)p=(iteration%6==0?colony():greedy(true));
            else {
                if(rng()%4!=0)p=sh.snapshot();
                if(iteration%3==1)ruin_recreate(p);
                else for(int q=0;q<std::max(8,g.n/16);q++) {
                    if(g.n<4)break;
                    int i=1+int(rng()%unsigned(g.n-3));
                    if(!g.precedes(p[i],p[i+1]))std::swap(p[i],p[i+1]);
                }
            }
        }
        sh.offer(p);
    }

};

// Exact dynamic programming over precedence ideals. This is particularly
// effective when many vertices are ordered, even if the instance is large.
// It merges equal states BEFORE expanding the next layer (unlike DFS history).
// A bounded attempt can safely fall back to B&B: no partial DP is a proof.
bool layered_dp(Shared& sh,Clock::time_point until,size_t memory) {
    const auto& g=sh.g;
    struct Node {std::vector<std::uint64_t> mask;Cost cost;size_t parent;int last;};
    const size_t cap=std::max<size_t>(1024,memory/(192+size_t(g.words)*16));
    std::vector<Node> arena;std::vector<size_t> layer;
    Node first{std::vector<std::uint64_t>(g.words),0,0,g.s};first.mask[g.s/64]|=std::uint64_t(1)<<(g.s%64);
    arena.push_back(std::move(first));layer.push_back(0);
    for(int depth=1;depth<g.n;depth++) {
        std::unordered_map<std::string,size_t> index;std::vector<size_t> next;
        for(size_t id:layer) {
            if(sh.expired() || Clock::now()>=until)return false;
            ++sh.nodes;Node current=arena[id];
            for(int b=0;b<g.n;b++) {
                if((current.mask[b/64]>>(b%64))&1)continue;
                bool ready=true;
                for(int w=0;w<g.words;w++)if(g.before[b][w]&~current.mask[w]){ready=false;break;}
                if(!ready)continue;
                Cost value=current.cost+g.c[current.last][b];
                if(value>=sh.best.load(std::memory_order_relaxed))continue;
                auto mask=current.mask;mask[b/64]|=std::uint64_t(1)<<(b%64);
                std::string key(size_t(g.words)*8+sizeof(b),'\0');
                std::memcpy(key.data(),mask.data(),size_t(g.words)*8);std::memcpy(key.data()+size_t(g.words)*8,&b,sizeof(b));
                auto found=index.find(key);
                if(found!=index.end()) {
                    auto& node=arena[found->second];if(value<node.cost){node.cost=value;node.parent=id;}
                } else {
                    if(arena.size()>=cap)return false;
                    size_t dest=arena.size();arena.push_back({std::move(mask),value,id,b});index.emplace(std::move(key),dest);next.push_back(dest);
                }
            }
        }
        if(next.empty())return true; // All paths unable to improve incumbent.
        layer=std::move(next);
    }
    size_t best=layer.front();for(size_t id:layer)if(arena[id].cost<arena[best].cost)best=id;
    std::vector<int> path;
    for(size_t id=best;;id=arena[id].parent){path.push_back(arena[id].last);if(id==0)break;}
    std::reverse(path.begin(),path.end());sh.offer(path);return true;
}

struct Task {
    std::vector<int> prefix;
    Assignment assignment;
    Cost cost;
    Cost bound()const{return cost+assignment.value;}
};
// Bounded, best-bound task pool with cooperative work donation. Idle workers
// request work; an active DFS donates unexplored siblings, including their
// repaired assignments. No subtree is dropped or recomputed from the root.
class Pool {
    Shared& sh;
    std::mutex lock;
    std::condition_variable changed;
    std::vector<Task> queue;
    std::vector<Cost> active;
    int working=0;
    Cost abandoned=INF;
    static bool worse(const Task& a,const Task& b){return a.bound()>b.bound();}
public:
    std::atomic<int> idle{0},queued{0};
    Pool(Shared& shared,int workers,Task root):sh(shared),active(workers,INF){queue.push_back(std::move(root));queued=1;}
    bool wants_work()const{return idle.load(std::memory_order_relaxed)>0 && queued.load(std::memory_order_relaxed)<2*idle.load(std::memory_order_relaxed);}
    void donate(Task task) {
        std::lock_guard<std::mutex> guard(lock);
        queue.push_back(std::move(task));std::push_heap(queue.begin(),queue.end(),worse);queued=int(queue.size());++sh.donations;changed.notify_one();
    }
    std::optional<Task> take(int worker) {
        std::unique_lock<std::mutex> guard(lock);
        while(queue.empty() && working>0 && !sh.expired()) {
            ++idle;changed.wait_until(guard,sh.deadline);--idle;
        }
        if(sh.expired() || queue.empty())return std::nullopt;
        std::pop_heap(queue.begin(),queue.end(),worse);Task result=std::move(queue.back());queue.pop_back();queued=int(queue.size());
        ++working;active[worker]=result.bound();return result;
    }
    void publish(int worker,Cost bound){std::lock_guard<std::mutex> guard(lock);active[worker]=std::max(active[worker],bound);}
    void finish(int worker,bool complete) {
        std::lock_guard<std::mutex> guard(lock);
        if(!complete)abandoned=std::min(abandoned,active[worker]);
        active[worker]=INF;--working;changed.notify_all();
    }
    void wake(){std::lock_guard<std::mutex> guard(lock);changed.notify_all();}
    Cost lower_bound() {
        std::lock_guard<std::mutex> guard(lock);Cost bound=abandoned;
        for(Cost b:active)bound=std::min(bound,b);
        if(!queue.empty())bound=std::min(bound,queue.front().bound());
        return bound;
    }
};
class Exact {
    Shared& sh;const Problem& g;Pool& pool;int worker;size_t capacity;bool interleave;
    std::uint64_t local_nodes=0;Heuristic improver;
    std::unordered_map<std::string,Cost> history;
    std::vector<std::uint64_t> mask;
    std::vector<unsigned char> used;
    std::vector<int> degree,path;
    std::vector<Cost> siblings;
    bool interrupted=false;
    std::string key(int last)const {
        std::string result(size_t(g.words)*8+sizeof(last),'\0');
        std::memcpy(result.data(),mask.data(),size_t(g.words)*8);
        std::memcpy(result.data()+size_t(g.words)*8,&last,sizeof(last));return result;
    }
    bool dominated(Cost prefix,const std::string& k) {
        if(capacity) {
            auto it=history.find(k);
            if(it!=history.end()) {
                if(it->second<=prefix){++sh.dominated;return true;}
                it->second=prefix;
            } else {
                if(history.size()>=capacity)history.clear();
                history.emplace(k,prefix);
            }
        }
        if(sh.history.test(k,prefix)){++sh.dominated;return true;}return false;
    }
    // A cheaper reordering of a prefix with the same endpoint dominates it,
    // even if that alternative has not yet been visited. Move the penultimate
    // vertex left across incomparable vertices; keep the endpoint fixed.
    bool local_dominated()const {
        int size=int(path.size());if(size<4)return false;
        int x=path[size-2],a=path[size-3],b=path[size-1];
        Cost removal=g.c[a][b]-g.c[a][x]-g.c[x][b];
        for(int j=size-4;j>=0;j--) {
            if(g.precedes(path[j+1],x))break;
            if(removal+g.c[path[j]][x]+g.c[x][path[j+1]]-g.c[path[j]][path[j+1]]<0)return true;
        }
        return false;
    }
    void push(int v){used[v]=1;mask[v/64]|=std::uint64_t(1)<<(v%64);path.push_back(v);for(int w:g.succ[v])--degree[w];}
    void pop(int v){for(int w:g.succ[v])++degree[w];path.pop_back();mask[v/64]&=~(std::uint64_t(1)<<(v%64));used[v]=0;}
    void publish(Cost current) {
        for(Cost sibling:siblings)current=std::min(current,sibling);
        pool.publish(worker,current);
    }
    void dfs(const Assignment& ap,Cost prefix) {
        if(sh.expired()){interrupted=true;return;}
        sh.nodes.fetch_add(1,std::memory_order_relaxed);++local_nodes;
        if(interleave && local_nodes%8192==0)improver.run(std::min(sh.deadline,Clock::now()+std::chrono::milliseconds(15)));
        if(local_nodes%512==0)publish(prefix+ap.value);
        int last=path.back();
        if(prefix+ap.value>=sh.best.load(std::memory_order_relaxed)){++sh.bound_prunes;return;}
        if(int(path.size())==g.n){sh.offer(path);return;}
        if(local_dominated()){++sh.local_prunes;return;}
        auto state=key(last);if(dominated(prefix,state))return;
        std::vector<std::pair<Cost,int>> choices;
        for(int b=0;b<g.n;b++)if(!used[b] && degree[b]==0 && g.arc[last][b]) {
            Cost reduced=g.c[last][b]-ap.u[last]-ap.v[b];
            if(prefix+ap.value+reduced<sh.best.load(std::memory_order_relaxed))choices.push_back({reduced,b});
            else ++sh.bound_prunes;
        }
        std::sort(choices.begin(),choices.end());std::vector<unsigned char> donated(choices.size());
        // Retain the most promising child for locality, expose siblings to idle workers.
        for(size_t i=1;i<choices.size() && pool.wants_work() && g.n-int(path.size())>=12;i++) {
            int b=choices[i].second;Assignment child=ap;
            if(!child.fix(last,b,sh)){if(sh.expired()){interrupted=true;return;}donated[i]=1;continue;}
            auto prefix_path=path;prefix_path.push_back(b);
            pool.donate({std::move(prefix_path),std::move(child),prefix+g.c[last][b]});donated[i]=1;
        }
        for(size_t i=0;i<choices.size();i++) {
            if(donated[i])continue;
            if(sh.history.test(state,prefix,true)){++sh.dominated;return;}
            int b=choices[i].second;Assignment child=ap;
            if(!child.fix(last,b,sh)){if(sh.expired()){interrupted=true;return;}continue;}
            Cost other=INF;
            for(size_t j=i+1;j<choices.size();j++)if(!donated[j]){other=prefix+ap.value+choices[j].first;break;}
            siblings.push_back(other);push(b);dfs(child,prefix+g.c[last][b]);pop(b);siblings.pop_back();
            if(interrupted)return;
        }
    }
public:
    Exact(Shared& s,Pool& p,int id,size_t budget,bool improve,std::uint64_t seed):sh(s),g(s.g),pool(p),worker(id),capacity(budget/(128+size_t(g.words)*8)),interleave(improve),improver(s,seed),mask(g.words),used(g.n),degree(g.indegree){}
    bool run(const Task& task) {
        degree=g.indegree;path.clear();siblings.clear();interrupted=false;
        std::fill(mask.begin(),mask.end(),0);std::fill(used.begin(),used.end(),0);
        for(int v:task.prefix)push(v);
        dfs(task.assignment,task.cost);return !interrupted;
    }
};
struct Result {Cost cost,lower;std::vector<int> path;std::uint64_t nodes,dominated,pruned,repairs;bool optimal;std::uint64_t local_prunes=0,donations=0;};
Result solve(const Input& in,Clock::time_point deadline,double limit,int threads,size_t memory,std::uint64_t seed,const std::string& engine="auto") {
    Problem g(in);Shared sh(g,deadline,memory*3/4);Heuristic first(sh,seed);sh.offer(first.greedy(false));
    size_t ordered=0;for(int a=0;a<in.n;a++)for(int b=0;b<in.n;b++)ordered+=g.precedes(a,b);
    if(engine=="dp" || (engine=="auto" && in.n>1 && double(ordered)>0.85*double(in.n)*double(in.n-1)/2)) {
        auto dp_end=std::min(deadline,Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::min(5.0,limit*0.25))));
        if(layered_dp(sh,engine=="dp"?deadline:dp_end,std::max<size_t>(8ULL*1024*1024,memory/2))) {
            auto path=sh.snapshot();Cost best=g.validate(path);
            return {best,best,std::vector<int>(path.begin()+1,path.end()-1),sh.nodes.load(),0,0,0,true};
        }
    }
    // Give sparse instances enough diversification to establish a strong
    // incumbent before exact search. Dense instances have already taken the
    // layered-DP route above, so this budget does not delay their proof.
    auto warm=std::min(deadline,Clock::now()+std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(std::min(2.0,std::max(0.05,limit*0.15)))));
    if(g.n>3)first.run(warm);
    Assignment root(g.n);bool initialized=root.initialize(sh);Cost lower=initialized?root.value:0;
    bool complete=false;std::exception_ptr worker_error;std::mutex error_lock;
    if(initialized && lower>=sh.best.load())complete=true;
    else if(initialized && !sh.expired()) {
        int exact_threads=threads>1?threads-1:1;Pool pool(sh,exact_threads,Task{{g.s},root,0});
        auto fail=[&](){std::lock_guard<std::mutex> guard(error_lock);if(!worker_error)worker_error=std::current_exception();sh.stop=true;pool.wake();};
        std::vector<std::thread> workers;
        struct Join {std::vector<std::thread>& threads;Shared& sh;Pool& pool;~Join(){sh.stop=true;pool.wake();for(auto& t:threads)if(t.joinable())t.join();}} join{workers,sh,pool};
        if(threads>1)workers.emplace_back([&](){try{Heuristic h(sh,seed+1);h.run(deadline);}catch(...){fail();}});
        for(int w=0;w<exact_threads;w++)workers.emplace_back([&,w](){try{
            Exact search(sh,pool,w,memory/4/size_t(exact_threads),threads==1,seed+7);
            while(auto task=pool.take(w)) {bool finished=search.run(*task);pool.finish(w,finished);if(!finished)break;}
        }catch(...){fail();}});
        for(size_t i=threads>1?1:0;i<workers.size();i++)workers[i].join();
        Cost frontier=pool.lower_bound();complete=frontier==INF;
        if(frontier!=INF)lower=std::max(lower,std::min(frontier,sh.best.load()));
        sh.stop=true;pool.wake();if(threads>1)workers[0].join();
    }
    if(worker_error)std::rethrow_exception(worker_error);
    auto path=sh.snapshot();Cost best=g.validate(path);if(best!=sh.best.load())throw std::runtime_error("Validation: reported cost");
    if(best==lower)complete=true;
    if(complete)lower=best;
    return {best,lower,std::vector<int>(path.begin()+1,path.end()-1),sh.nodes.load(),sh.dominated.load(),sh.bound_prunes.load(),sh.repairs.load(),complete,sh.local_prunes.load(),sh.donations.load()};
}
std::string csv(std::string s) {
    std::string r="\"";for(char c:s) {r+=c;if(c=='"') r+='"';} return r+'"';
}
#ifndef SOP_LIBRARY
int main(int argc,char** argv) {
    try {
        double limit=60;int threads=int(std::min(4U,std::max(1U,std::thread::hardware_concurrency())));
        size_t memory=256ULL*1024*1024;std::uint64_t seed=20261012;std::string engine="auto",output;std::vector<std::string> files;bool positional=false;
        for(int i=1;i<argc;i++) {
            std::string a=argv[i];
            if(!positional && a=="--") {positional=true;continue;}
            if(!positional && (a=="--help" || a=="-h")) {
                std::cout<<"Usage: sop_solver [--time-limit SECONDS] [--output results.csv] [--threads N] [--memory-mb MB] [--seed N] [--engine auto|bnb|dp] FILE.sop ...\n"
                           "Default: 60 seconds per file. CSV on stdout; status on stderr.\n";return 0;
            }
            if(!positional && (a=="--time-limit" || a=="-t" || a=="--output" || a=="-o" || a=="--threads" || a=="--memory-mb" || a=="--seed" || a=="--engine")) {
                if(++i==argc) throw std::runtime_error("Missing option value");
                if(a=="-o" || a=="--output") output=argv[i];
                else if(a=="--engine") {engine=argv[i];if(engine!="auto" && engine!="bnb" && engine!="dp")throw std::runtime_error("Engine must be auto, bnb, or dp");}
                else if(a=="--threads" || a=="--memory-mb" || a=="--seed") {
                    Cost value=integer(argv[i]);
                    if(a=="--threads") {if(value<1 || value>64)throw std::runtime_error("Threads must be 1..64");threads=int(value);}
                    else if(a=="--memory-mb") {if(value<0 || value>65536)throw std::runtime_error("Memory budget must be 0..65536 MiB");memory=size_t(value)*1024*1024;}
                    else {if(value<0)throw std::runtime_error("Seed must be nonnegative");seed=std::uint64_t(value);}
                }
                else {size_t k;std::string s=argv[i];limit=std::stod(s,&k);
                    if(k!=s.size() || !std::isfinite(limit) || limit<0 || limit>31536000)
                        throw std::runtime_error("Time limit must be finite, 0..31536000 seconds");}
            } else if(!positional && !a.empty() && a[0]=='-') throw std::runtime_error("Unknown option: "+a);
            else files.push_back(a);
        }
        if(files.empty()) throw std::runtime_error("No input files; use --help");
        std::ofstream of;std::ostream* out=&std::cout;
        if(!output.empty()) {
            for(const auto& file:files) {
                if(std::filesystem::weakly_canonical(output)==std::filesystem::weakly_canonical(file) ||
                   (std::filesystem::exists(output) && std::filesystem::exists(file) && std::filesystem::equivalent(output,file)))
                    throw std::runtime_error("Output must not overwrite an input file");
            }
            of.open(output);if(!of) throw std::runtime_error("Cannot open output file");out=&of;
        }
        *out<<"Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path\n";
        bool failed=false;
        for(const auto& file:files) {
            auto begin=Clock::now();int n=0;
            try {
                Input input=read_sop(file);n=input.n;
                auto deadline=begin+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(limit));
                auto result=solve(input,deadline,limit,threads,memory,seed,engine);const auto& p=result.path;
                double elapsed=std::chrono::duration<double>(Clock::now()-begin).count();
                std::ostringstream path;for(size_t i=0;i<p.size();i++) {if(i) path<<' ';path<<p[i];}
                *out<<csv(std::filesystem::path(file).filename().string())<<','<<n<<','<<std::fixed<<std::setprecision(6)
                    <<elapsed<<','<<result.nodes<<','<<result.cost<<','<<csv(path.str())<<'\n';
                std::cerr<<file<<": "<<(result.optimal?"OPTIMAL (proven)":"TIME_LIMIT (best found; not proven optimal)")
                         <<"; cost="<<result.cost<<"; start="<<p.front()<<"; end="<<p.back()<<"; lower_bound="<<result.lower
                         <<"; gap_percent="<<(result.cost?100.0*double(result.cost-result.lower)/double(result.cost):0.0)
                         <<"; dominated="<<result.dominated<<"; bound_prunes="<<result.pruned
                         <<"; assignment_repairs="<<result.repairs<<"; local_prunes="<<result.local_prunes<<"; donated_tasks="<<result.donations<<'\n';
            } catch(const std::exception& e) {
                failed=true;std::cerr<<file<<": ERROR: "<<e.what()<<'\n';
                *out<<csv(std::filesystem::path(file).filename().string())<<',';
                if(n) *out<<n;
                *out<<','<<std::fixed<<std::setprecision(6)<<std::chrono::duration<double>(Clock::now()-begin).count()<<",0,,\n";
            }
            out->flush();if(!*out) throw std::runtime_error("Failed writing CSV");
        }
        return failed?1:0;
    } catch(const std::exception& e) {std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}
}

#endif

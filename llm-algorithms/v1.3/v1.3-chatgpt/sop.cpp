// Exact SOP solver. C++17 / OpenMP. See README.md for proof and limits.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <omp.h>
using I = int64_t;
constexpr I INF = std::numeric_limits<I>::max()/4;
using Clock = std::chrono::steady_clock;
struct Options {
    std::string file, output;
    double limit=0; int threads=1; bool header=true;
    size_t history_mb=16; unsigned seed=1; int starts=32;
};
I integer(const std::string& s) {
    size_t p=0; I x=std::stoll(s,&p); if(p!=s.size()) throw std::runtime_error("Invalid integer: "+s); return x;
}
Options options(int argc,char** argv) {
    Options o;
    for(int i=1;i<argc;++i) {
        std::string a=argv[i];
        auto value=[&](){ if(++i==argc) throw std::runtime_error("Missing value for "+a); return std::string(argv[i]); };
        if(a=="--time-limit") { auto s=value(); size_t p; o.limit=std::stod(s,&p); if(p!=s.size() || !std::isfinite(o.limit) || o.limit<=0) throw std::runtime_error("Time limit must be finite and positive"); }
        else if(a=="--threads") { I t=integer(value()); if(t<1 || t>4096) throw std::runtime_error("Threads must be 1..4096"); o.threads=int(t); }
        else if(a=="--history-mb") { I t=integer(value()); if(t<0 || t>1048576) throw std::runtime_error("Invalid history budget"); o.history_mb=size_t(t); }
        else if(a=="--heuristic-starts") { I t=integer(value()); if(t<1 || t>1000000) throw std::runtime_error("Invalid heuristic starts"); o.starts=int(t); }
        else if(a=="--seed") { I t=integer(value()); if(t<0 || t>4294967295LL) throw std::runtime_error("Invalid seed"); o.seed=unsigned(t); }
        else if(a=="--output") o.output=value();
        else if(a=="--no-header") o.header=false;
        else if(a=="--help") { std::cout<<"Usage: sop FILE [--time-limit SECONDS] [--threads N] [--output CSV] [--no-header]\n  [--history-mb MB_PER_THREAD] [--heuristic-starts N] [--seed N]\n"; std::exit(0); }
        else if(a.rfind("--",0)==0 || !o.file.empty()) throw std::runtime_error("Unexpected argument: "+a);
        else o.file=a;
    }
    if(o.file.empty()) throw std::runtime_error("Missing .sop file; use --help");
    if(!o.output.empty() && std::filesystem::weakly_canonical(o.file)==std::filesystem::weakly_canonical(o.output)) throw std::runtime_error("Output must not overwrite input");
    return o;
}
struct Instance {
    int n=0, words=0;
    std::vector<I> original, edge;
    std::vector<std::vector<int>> succ;
    std::vector<int> degree;
    std::vector<std::vector<uint64_t>> pred, reach;
    I raw(int i,int j) const { return original[size_t(i)*n+j]; }
    bool before(int i,int j) const { return (reach[i][j/64]>>(j%64))&1; }
    I cost(int i,int j) const { return edge[size_t(i)*(n+1)+j]; }
    void read(const std::string& name) {
        std::ifstream f(name); if(!f) throw std::runtime_error("Cannot open "+name);
        std::string line; bool section=false, dimension=false;
        while(std::getline(f,line)) {
            auto colon=line.find(':'); if(colon!=std::string::npos) line[colon]=' ';
            std::istringstream ss(line); std::string key,val; ss>>key;
            if(key=="EDGE_WEIGHT_SECTION") { section=true; break; }
            if(key=="DIMENSION") { if(dimension || !(ss>>val)) throw std::runtime_error("Invalid DIMENSION"); I t=integer(val); if(t<1 || t>1000) throw std::runtime_error("DIMENSION must be 1..1000"); n=int(t); dimension=true; }
            else if(key=="TYPE" || key=="EDGE_WEIGHT_TYPE" || key=="EDGE_WEIGHT_FORMAT") {
                if(!(ss>>val) || (key=="TYPE" && val!="SOP") || (key=="EDGE_WEIGHT_TYPE" && val!="EXPLICIT") || (key=="EDGE_WEIGHT_FORMAT" && val!="FULL_MATRIX")) throw std::runtime_error("Unsupported "+key+": "+val);
            } // NAME, COMMENT and extension metadata do not affect the graph.
        }
        if(!section || !dimension) throw std::runtime_error("Missing DIMENSION or EDGE_WEIGHT_SECTION");
        std::string s; if(!(f>>s) || integer(s)!=n) throw std::runtime_error("Missing or mismatched repeated dimension");
        original.resize(size_t(n)*n);
        // Bound all sums and dual arithmetic well away from the INF sentinel.
        I maxcost=INF/(8*(n+1));
        for(auto& x:original) { if(!(f>>s) || s=="EOF") throw std::runtime_error("Truncated matrix"); x=integer(s); if(x>maxcost) throw std::runtime_error("Cost exceeds safe 64-bit range for this dimension"); }
        if(f>>s) { if(s!="EOF") throw std::runtime_error("Extra data after matrix"); if(f>>s) throw std::runtime_error("Extra data after EOF"); }
        for(int i=0;i<n;++i) if(raw(i,i)!=0) throw std::runtime_error("Diagonal must be zero");
    }
};
struct Matching {
    std::vector<int> row,col;
    std::vector<I> u,v;
    I bound=0;
    explicit Matching(int m=0):row(m,-1),col(m,-1),u(m),v(m) {}
};
struct State {
    std::vector<int> remaining,degree,path;
    std::vector<uint64_t> visited;
    Matching matching;
    int last; I paid=0;
    explicit State(const Instance& g):degree(g.degree),visited(g.words),matching(g.n+1),last(g.n) {
        remaining.resize(g.n); std::iota(remaining.begin(),remaining.end(),0);
    }
};
struct alignas(128) Worker {
    std::unordered_map<std::string,I> history;
    uint64_t nodes=0, dominated=0, bound_pruned=0;
};
class Solver {
    const Options& opt;
    Clock::time_point start;
    std::atomic<bool> stopped{false};
    std::atomic<I> incumbent{INF};
    std::atomic<int> tasks{0};
    std::mutex best_mutex, error_mutex;
    std::vector<int> best;
    std::string error;
    std::unique_ptr<Worker[]> workers;
    size_t history_cap=0;
public:
    Instance g;
    bool complete=false, cycle=false;
    Solver(const Options& o,Clock::time_point t):opt(o),start(t),workers(new Worker[o.threads]) {}
    double elapsed() const { return std::chrono::duration<double>(Clock::now()-start).count(); }
    bool expired() {
        if(stopped.load(std::memory_order_relaxed)) return true;
        if(opt.limit>0 && elapsed()>=opt.limit) { stopped.store(true,std::memory_order_relaxed); return true; }
        return false;
    }
    bool preprocess() {
        int n=g.n; g.words=(n+63)/64;
        g.succ.resize(n); g.degree.assign(n,0);
        g.pred.assign(n,std::vector<uint64_t>(g.words)); g.reach=g.pred;
        for(int i=0;i<n;++i) for(int j=0;j<n;++j) if(i!=j && g.raw(i,j)<0) { g.succ[j].push_back(i); ++g.degree[i]; }
        auto d=g.degree; std::vector<int> top;
        for(int i=0;i<n;++i) if(!d[i]) top.push_back(i);
        for(size_t k=0;k<top.size();++k) for(int v:g.succ[top[k]]) if(--d[v]==0) top.push_back(v);
        if(int(top.size())!=n) { cycle=true; return false; }
        for(int v:top) {
            if(expired()) return false;
            for(int w:g.succ[v]) { for(int k=0;k<g.words;++k) g.pred[w][k]|=g.pred[v][k]; g.pred[w][v/64]|=1ULL<<(v%64); }
        }
        for(int i=0;i<n;++i) for(int j=0;j<n;++j) if((g.pred[j][i/64]>>(i%64))&1) g.reach[i][j/64]|=1ULL<<(j%64);
        g.edge.assign(size_t(n+1)*(n+1),INF);
        for(int i=0;i<n;++i) {
            if(expired()) return false;
            for(int j=0;j<n;++j) if(i!=j && !g.before(j,i)) {
                bool between=false;
                for(int k=0;k<g.words;++k) if(g.reach[i][k]&g.pred[j][k]) { between=true; break; }
                if(!between) g.edge[size_t(i)*(n+1)+j]=g.raw(i,j);
            }
            if(g.succ[i].empty()) g.edge[size_t(i)*(n+1)+n]=0;
            if(!g.degree[i]) g.edge[size_t(n)*(n+1)+i]=0;
        }
        history_cap=opt.history_mb*size_t(1024)*1024/(96+g.words*8);
        return !expired();
    }
    I validate(const std::vector<int>& p) const {
        if(int(p.size())!=g.n) throw std::runtime_error("Internal error: incomplete path");
        std::vector<int> pos(g.n,-1); I c=0;
        for(int k=0;k<g.n;++k) { int v=p[k]; if(v<0 || v>=g.n || pos[v]!=-1) throw std::runtime_error("Internal error: invalid permutation"); pos[v]=k; }
        for(int i=0;i<g.n;++i) for(int j=0;j<g.n;++j) if(i!=j && g.raw(i,j)<0 && pos[j]>=pos[i]) throw std::runtime_error("Internal error: precedence violation");
        for(int k=1;k<g.n;++k) { I e=g.raw(p[k-1],p[k]); if(e<0) throw std::runtime_error("Internal error: negative path arc"); c+=e; }
        return c;
    }
    void publish(const std::vector<int>& p,I c) {
        if(c>=incumbent.load(std::memory_order_relaxed)) return;
        if(validate(p)!=c) throw std::runtime_error("Internal error: cost mismatch");
        std::lock_guard<std::mutex> lock(best_mutex);
        if(c<incumbent.load(std::memory_order_relaxed)) { best=p; incumbent.store(c,std::memory_order_relaxed); }
    }
    void fail(const std::string& msg) {
        std::lock_guard<std::mutex> lock(error_mutex); if(error.empty()) error=msg; stopped=true;
    }
    // Primal-dual augmenting path: retained potentials are feasible after row/column deletion.
    bool augment(State& state,int root,const std::vector<int>& cols) {
        auto& m=state.matching;
        int z=g.n+1; std::vector<I> slack(z,INF); std::vector<int> prev(z,-1);
        std::vector<unsigned char> used(z,0); int r=root, previous=-1;
        for(;;) {
            if(expired()) return false;
            I delta=INF; int next=-1;
            for(int c:cols) if(!used[c]) {
                I edge=g.cost(r,c);
                if(r==state.last && ((c<g.n && state.degree[c]>0) || (c==g.n && !state.remaining.empty()))) edge=INF;
                if(edge<INF) { I reduced=edge-m.u[r]-m.v[c]; if(reduced<slack[c]) { slack[c]=reduced; prev[c]=previous; } }
                if(slack[c]<delta) { delta=slack[c]; next=c; }
            }
            if(next<0) return false;
            m.u[root]+=delta;
            for(int c:cols) {
                if(used[c]) { m.u[m.col[c]]+=delta; m.v[c]-=delta; }
                else if(slack[c]<INF) slack[c]-=delta;
            }
            used[next]=1;
            if(m.col[next]<0) {
                int c=next;
                while(c>=0) { int p=prev[c]; int rr=p<0?root:m.col[p]; m.col[c]=rr; m.row[rr]=c; c=p; }
                return true;
            }
            previous=next; r=m.col[next];
        }
    }
    bool root_matching(State& s) {
        auto cols=s.remaining; cols.push_back(g.n);
        for(int r=0;r<=g.n;++r) if(!augment(s,r,cols)) return false;
        s.matching.bound=0;
        for(int r=0;r<=g.n;++r) s.matching.bound+=g.cost(r,s.matching.row[r]);
        return true;
    }
    bool advance(State& s,int v) {
        int old=s.last; auto& m=s.matching;
        int freed=m.row[old], unmatched=m.col[v];
        m.row[old]=-1; m.col[v]=-1;
        s.paid+=g.cost(old,v); s.last=v; s.path.push_back(v);
        s.visited[v/64]|=1ULL<<(v%64);
        s.remaining.erase(std::find(s.remaining.begin(),s.remaining.end(),v));
        for(int w:g.succ[v]) --s.degree[w];
        std::vector<int> repair;
        if(freed!=v) {
            m.col[freed]=-1; m.row[unmatched]=-1; repair.push_back(unmatched);
        }
        int target=m.row[v];
        if(target>=0 && ((target<g.n && s.degree[target]>0) || (target==g.n && !s.remaining.empty()))) {
            m.row[v]=-1; m.col[target]=-1; repair.push_back(v);
        }
        auto cols=s.remaining; cols.push_back(g.n);
        for(int r:repair) if(!augment(s,r,cols)) return false;
        m.bound=g.cost(v,m.row[v]);
        for(int r:s.remaining) m.bound+=g.cost(r,m.row[r]);
        return true;
    }
    // Or-opt style relocation of 1..3-node blocks, preserving precedence.
    void improve(std::vector<int>& p) {
        int n=g.n;
        auto e=[&](int a,int b)->I { return a<0 || b<0 ? 0:g.raw(a,b); };
        for(int round=0;round<8 && !expired();++round) {
            bool changed=false;
            std::vector<int> pos(n); for(int k=0;k<n;++k) pos[p[k]]=k;
            for(int len=1;len<=std::min(3,n-1);++len) {
                for(int i=0;i+len<=n && !expired();++i) {
                    int lo=0,hi=n-len;
                    for(int y=0;y<n;++y) if(pos[y]<i || pos[y]>=i+len) {
                        int reduced=pos[y]-(pos[y]>=i+len?len:0);
                        for(int k=0;k<len;++k) {
                            if(g.before(y,p[i+k])) lo=std::max(lo,reduced+1);
                            if(g.before(p[i+k],y)) hi=std::min(hi,reduced);
                        }
                    }
                    int a=i?p[i-1]:-1,b=i+len<n?p[i+len]:-1;
                    int first=p[i],last=p[i+len-1];
                    I removal=e(a,b)-e(a,first)-e(last,b),bestdelta=0;
                    int target=i;
                    auto at=[&](int j){ return j<0 || j>=n-len?-1:p[j+(j>=i?len:0)]; };
                    for(int j=lo;j<=hi;++j) if(j!=i) {
                        int l=at(j-1),r=at(j);
                        I delta=removal+e(l,first)+e(last,r)-e(l,r);
                        if(delta<bestdelta) { bestdelta=delta; target=j; }
                    }
                    if(target!=i) {
                        std::vector<int> block(p.begin()+i,p.begin()+i+len);
                        p.erase(p.begin()+i,p.begin()+i+len);
                        p.insert(p.begin()+target,block.begin(),block.end());
                        for(int k=0;k<n;++k) pos[p[k]]=k;
                        changed=true;
                    }
                }
            }
            if(!changed) break;
        }
    }
    void heuristic(int run) {
        std::mt19937 rng(opt.seed+unsigned(run)*2654435761U);
        auto d=g.degree; std::vector<int> ready,p;
        for(int v=0;v<g.n;++v) if(!d[v]) ready.push_back(v);
        int last=g.n;
        while(!ready.empty()) {
            if(expired()) return;
            std::sort(ready.begin(),ready.end(),[&](int a,int b){ I ca=g.cost(last,a),cb=g.cost(last,b); return ca!=cb?ca<cb:a<b; });
            size_t k=0;
            if(run>0 && rng()%100<25) k=rng()%std::min<size_t>(ready.size(),4);
            int v=ready[k]; ready.erase(ready.begin()+k); p.push_back(v); last=v;
            for(int w:g.succ[v]) if(--d[w]==0) ready.push_back(w);
        }
        publish(p,validate(p)); improve(p); publish(p,validate(p));
    }
    void branch(std::shared_ptr<const State> parent,int v) noexcept {
        try {
            if(expired()) return;
            auto s=std::make_shared<State>(*parent);
            ++workers[omp_get_thread_num()].nodes;
            if(advance(*s,v)) search(s);
        } catch(const std::exception& e) { fail(e.what()); }
    }
    void search(std::shared_ptr<const State> s) {
        if(expired()) return;
        auto& w=workers[omp_get_thread_num()];
        I upper=incumbent.load(std::memory_order_relaxed);
        if(s->paid+s->matching.bound>=upper) { ++w.bound_pruned; return; }
        if(s->remaining.empty()) { publish(s->path,s->paid); return; }
        if(history_cap) {
            std::string key(reinterpret_cast<const char*>(s->visited.data()),s->visited.size()*sizeof(uint64_t));
            key.push_back(char(s->last&255)); key.push_back(char(s->last>>8));
            auto it=w.history.find(key);
            if(it!=w.history.end()) { if(it->second<=s->paid) { ++w.dominated; return; } it->second=s->paid; }
            else { if(w.history.size()>=history_cap) w.history.clear(); w.history.emplace(std::move(key),s->paid); }
        }
        // If the optimal assignment is itself a topological Hamiltonian path,
        // it attains the bound and solves this subtree without branching.
        {
            auto p=s->path; auto d=s->degree; auto seen=s->visited; int r=s->last; bool valid=true;
            while(int(p.size())<g.n) {
                int v=s->matching.row[r];
                if(v<0 || v>=g.n || ((seen[v/64]>>(v%64))&1) || d[v]) { valid=false; break; }
                p.push_back(v); seen[v/64]|=1ULL<<(v%64); for(int q:g.succ[v]) --d[q]; r=v;
            }
            if(valid && s->matching.row[r]==g.n) { publish(p,s->paid+s->matching.bound); return; }
        }
        std::vector<std::pair<I,int>> choices;
        for(int v:s->remaining) if(!s->degree[v]) {
            I c=g.cost(s->last,v); if(c==INF) continue;
            I rc=c-s->matching.u[s->last]-s->matching.v[v];
            if(s->paid+s->matching.bound+rc<upper) choices.emplace_back(rc,v);
        }
        std::sort(choices.begin(),choices.end(),[&](auto a,auto b) {
            if(a.first!=b.first) return a.first<b.first;
            I ca=g.cost(s->last,a.second),cb=g.cost(s->last,b.second);
            return ca!=cb?ca<cb:a.second<b.second;
        });
        // Coarse tasks are capped; tied OpenMP tasks retain worker-local histories.
        for(size_t k=1;k<choices.size() && !expired();++k) {
            int v=choices[k].second; bool spawn=false;
            if(opt.threads>1 && s->remaining.size()>5) {
                int count=tasks.load(std::memory_order_relaxed);
                while(count<8*opt.threads) if(tasks.compare_exchange_weak(count,count+1,std::memory_order_relaxed)) { spawn=true; break; }
            }
            if(spawn) {
                #pragma omp task firstprivate(s,v)
                { branch(s,v); tasks.fetch_sub(1,std::memory_order_relaxed); }
            } else {
                // Preserve best-first DFS when task budget is full.
                for(size_t j=k;j<choices.size();++j) choices[j].first=-1;
                break;
            }
        }
        if(!choices.empty()) branch(s,choices[0].second);
        for(size_t k=1;k<choices.size() && !expired();++k) if(choices[k].first==-1) branch(s,choices[k].second);
    }
    void run() {
        if(!preprocess()) return;
        omp_set_dynamic(0); omp_set_num_threads(opt.threads);
        heuristic(0); // Feasible incumbent before spending time on matching.
        if(expired()) return;
        auto root=std::make_shared<State>(g);
        if(!root_matching(*root)) { if(!expired()) throw std::runtime_error("Internal error: feasible instance has no root matching"); return; }
        std::cerr<<"Root assignment lower bound: "<<root->matching.bound<<"\n";
        if(root->matching.bound<incumbent.load() && !expired()) {
            #pragma omp parallel
            {
                #pragma omp for schedule(dynamic,1)
                for(int run=1;run<opt.starts;++run) {
                    try { if(!expired()) heuristic(run); } catch(const std::exception& e) { fail(e.what()); }
                }
                #pragma omp single
                {
                    try { ++workers[omp_get_thread_num()].nodes; search(root); }
                    catch(const std::exception& e) { fail(e.what()); }
                }
            } // implicit barrier includes all descendant tasks
        }
        if(!error.empty()) throw std::runtime_error(error);
        complete=!expired();
    }
    void report(const Options& o) {
        if(!best.empty() && validate(best)!=incumbent.load()) throw std::runtime_error("Final validation failed");
        uint64_t nodes=0,dom=0,prune=0;
        for(int t=0;t<o.threads;++t) { nodes+=workers[t].nodes; dom+=workers[t].dominated; prune+=workers[t].bound_pruned; }
        bool optimal=complete && !best.empty() && !expired();
        double seconds=elapsed();
        std::cerr<<(cycle?"Infeasible: precedence cycle":optimal?"Optimality proved":"Stopped without an optimality proof")<<"; history prunes="<<dom<<", bound prunes="<<prune<<"\n";
        auto quote=[](const std::string& s){ std::string r="\""; for(char c:s) { if(c=='\"') r+='\"'; r+=c; } return r+'\"'; };
        const std::string header="Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal\n";
        std::ostringstream path,row;
        for(size_t k=0;k<best.size();++k) { if(k) path<<' '; path<<best[k]; }
        row<<quote(std::filesystem::path(o.file).filename().string())<<','<<g.n<<','<<std::fixed<<std::setprecision(6)<<seconds<<','<<nodes<<','<<(best.empty()?-1:incumbent.load())<<','<<quote(path.str())<<','<<(optimal?"yes":"no")<<'\n';
        if(!o.output.empty()) {
            bool empty=!std::filesystem::exists(o.output) || std::filesystem::file_size(o.output)==0;
            std::ofstream f(o.output,std::ios::app); if(!f) throw std::runtime_error("Cannot open output CSV");
            if(empty && o.header) f<<header;
            f<<row.str(); f.flush(); if(!f) throw std::runtime_error("Cannot write output CSV");
        }
        if(o.header) std::cout<<header;
        std::cout<<row.str(); std::cout.flush(); if(!std::cout) throw std::runtime_error("Cannot write stdout");
    }
};
int main(int argc,char** argv) {
    try { auto o=options(argc,argv); auto start=Clock::now(); Solver s(o,start); s.g.read(o.file); s.run(); s.report(o); return 0; }
    catch(const std::exception& e) { std::cerr<<"Error: "<<e.what()<<'\n'; return 2; }
}

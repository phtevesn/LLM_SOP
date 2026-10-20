// SOP Exact: integer assignment branch-and-bound, parallel prefix search.
// Copyright (c) 2026. Released under the MIT license; see LICENSE.
#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
using I = int64_t;
using Wide = __int128_t;
constexpr Wide WINF = Wide(1)<<120;
using Clock = std::chrono::steady_clock;
constexpr I INF = std::numeric_limits<I>::max()/4;
static std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);
static void signal_handler(int) { interrupted.store(true,std::memory_order_relaxed); }
struct TimedOut {};
struct SearchDone {};
struct Control {
    Clock::time_point start = Clock::now();
    double limit = std::numeric_limits<double>::infinity();
    std::atomic<bool> stop{false};
    double elapsed() const { return std::chrono::duration<double>(Clock::now()-start).count(); }
    bool expired() {
        if (stop.load(std::memory_order_relaxed)) return true;
        if (interrupted.load(std::memory_order_relaxed) || elapsed() >= limit) { stop.store(true); return true; }
        return false;
    }
    void check() { if (expired()) throw TimedOut{}; }
};
struct Options {
    std::string input, output;
    int threads=1;
    size_t history_mb=512;
    bool header=true, pin=true;
    uint64_t seed=1;
    std::string strategy="portfolio";
    bool shared_history=true;
};
static I integer(const std::string& s) {
    I x; const char* p=s.data(); const char* end=p+s.size();
    if (p!=end && *p=='+') ++p;
    auto r=std::from_chars(p,end,x);
    if (r.ec!=std::errc{} || r.ptr!=end || p==end) throw std::runtime_error("Invalid integer: "+s);
    return x;
}
static std::string trim(std::string s) {
    auto a=s.find_first_not_of(" \t\r\n"); if(a==s.npos) return "";
    return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
static std::string upper(std::string s) { for(auto& c:s) c=char(std::toupper(static_cast<unsigned char>(c))); return s; }
static void usage() {
    std::cerr << "Usage: sop FILE [--time-limit SECONDS] [--threads N] [--output FILE]\n"
                 "           [--no-header] [--history-mb M] [--seed N] [--no-pin]\n           [--strategy portfolio|prefix|arcs] [--history-mode shared|local]\n";
}
static Options options(int argc,char** argv,Control& ctl) {
    Options o;
    for(int i=1;i<argc;++i) {
        std::string a=argv[i];
        auto val=[&]() -> std::string {if(++i>=argc) throw std::runtime_error("Missing value for "+a); return argv[i];};
        if(a=="--help" || a=="-h") {usage(); std::exit(0);}
        else if(a=="--time-limit") {auto v=val(); size_t k=0; ctl.limit=std::stod(v,&k); if(k!=v.size() || !std::isfinite(ctl.limit) || ctl.limit<0) throw std::runtime_error("Invalid time limit");}
        else if(a=="--threads") {I n=integer(val()); if(n<1 || n>4096) throw std::runtime_error("Threads must be 1..4096"); o.threads=int(n);}
        else if(a=="--history-mb") {I n=integer(val()); if(n<0 || n>1048576) throw std::runtime_error("Invalid history size"); o.history_mb=size_t(n);}
        else if(a=="--seed") {I n=integer(val()); if(n<0) throw std::runtime_error("Seed must be nonnegative"); o.seed=uint64_t(n);}
        else if(a=="--history-mode") {auto v=val();if(v!="local" && v!="shared") throw std::runtime_error("History mode must be shared or local");o.shared_history=v=="shared";}
        else if(a=="--strategy") {o.strategy=val();if(o.strategy!="portfolio" && o.strategy!="prefix" && o.strategy!="arcs") throw std::runtime_error("Unknown strategy");}
        else if(a=="--output") o.output=val();
        else if(a=="--no-header") o.header=false;
        else if(a=="--no-pin") o.pin=false;
        else if(!a.empty() && a[0]=='-') throw std::runtime_error("Unknown option: "+a);
        else if(o.input.empty()) o.input=a;
        else throw std::runtime_error("Only one input file is accepted");
    }
    if(o.input.empty()) throw std::runtime_error("Input file required");
    if(!o.output.empty() && std::filesystem::weakly_canonical(o.input)==std::filesystem::weakly_canonical(o.output)) throw std::runtime_error("Output must not overwrite input");
    return o;
}
struct Problem {
    int n=0, m=0, words=0;
    std::vector<I> raw,cost;
    std::vector<std::vector<int>> pred,succ;
    std::vector<std::vector<uint64_t>> ancestors,descendants;
    std::vector<int> topo;
    bool cyclic=false,unique_order=true;
    I original(int a,int b) const {return a==n || b==n ? 0 : raw[size_t(a)*n+b];}
    void read(const std::string& file,Control& ctl) {
        std::ifstream in(file); if(!in) throw std::runtime_error("Cannot open input: "+file);
        std::string line; bool section=false;
        while(std::getline(in,line)) {
            line=trim(line); if(line.empty()) continue;
            auto k=line.find_first_of(": \t");
            auto key=upper(line.substr(0,k));
            auto value=k==line.npos?"":trim(line.substr(k+1));
            if(!value.empty() && value[0]==':') value=trim(value.substr(1));
            if(key=="DIMENSION") {I v=integer(value); if(n || v<1 || v>1000) throw std::runtime_error("DIMENSION must occur once, in 1..1000"); n=int(v);}
            if(key=="TYPE" && upper(value)!="SOP") throw std::runtime_error("TYPE must be SOP");
            if(key=="EDGE_WEIGHT_TYPE" && upper(value)!="EXPLICIT") throw std::runtime_error("Only EXPLICIT costs supported");
            if(key=="EDGE_WEIGHT_FORMAT" && upper(value)!="FULL_MATRIX") throw std::runtime_error("Only FULL_MATRIX supported");
            if(key=="EDGE_WEIGHT_SECTION") {section=true; break;}
            if(n) ctl.check();
        }
        if(!section || !n) throw std::runtime_error("Missing DIMENSION or EDGE_WEIGHT_SECTION");
        m=n+1; words=(n+63)/64;
        std::string token;
        if(!(in>>token) || integer(token)!=n) throw std::runtime_error("Missing or inconsistent repeated dimension");
        raw.resize(size_t(n)*n); pred.resize(n); succ.resize(n);
        for(int i=0;i<n;++i) {
            ctl.check();
            for(int j=0;j<n;++j) {
                if(!(in>>token)) throw std::runtime_error("Truncated matrix");
                I x=integer(token);
                if(x>INF/(8LL*m)) throw std::runtime_error("Cost too large for safe signed 64-bit bounds");
                raw[size_t(i)*n+j]=i==j?0:x;
                if(i!=j && x<0) {pred[i].push_back(j); succ[j].push_back(i);}
            }
        }
        if(in>>token) {if(upper(token)!="EOF" || (in>>token)) throw std::runtime_error("Unexpected data after matrix");}
        std::vector<int> d(n); std::deque<int> ready;
        for(int i=0;i<n;++i) {d[i]=int(pred[i].size()); if(!d[i]) ready.push_back(i);}
        while(!ready.empty()) {if(ready.size()>1) unique_order=false;int x=ready.front();ready.pop_front();topo.push_back(x);for(int y:succ[x]) if(--d[y]==0) ready.push_back(y);}
        cyclic=int(topo.size())!=n;
    }
    void preprocess(Control& ctl) {
        ancestors.assign(n,std::vector<uint64_t>(words)); descendants=ancestors;
        for(int x:topo) {
            ctl.check();
            for(int p:pred[x]) {
                ancestors[x][p/64]|=uint64_t(1)<<(p%64);
                for(int w=0;w<words;++w) ancestors[x][w]|=ancestors[p][w];
            }
        }
        for(int i=0;i<n;++i) for(int j=0;j<n;++j)
            if((ancestors[i][j/64]>>(j%64))&1) descendants[j][i/64]|=uint64_t(1)<<(i%64);
        cost.assign(size_t(m)*m,INF);
        for(int i=0;i<n;++i) {
            ctl.check();
            for(int j=0;j<n;++j) {
                if(i==j || ((ancestors[i][j/64]>>(j%64))&1)) continue;
                bool middle=false;
                for(int w=0;w<words;++w) if(descendants[i][w]&ancestors[j][w]) {middle=true;break;}
                if(!middle) cost[size_t(i)*m+j]=raw[size_t(i)*n+j];
            }
            if(pred[i].empty()) cost[size_t(n)*m+i]=0;
            if(succ[i].empty()) cost[size_t(i)*m+n]=0;
        }
    }
    I validate(const std::vector<int>& path) const {
        if(int(path.size())!=n) throw std::runtime_error("Internal error: incomplete path");
        std::vector<int> pos(n,-1);
        for(int k=0;k<n;++k) {int x=path[k];if(x<0 || x>=n || pos[x]!=-1) throw std::runtime_error("Internal error: path is not a permutation");pos[x]=k;}
        I total=0;
        for(int i=0;i<n;++i) for(int j:pred[i]) if(pos[j]>=pos[i]) throw std::runtime_error("Internal error: precedence violation");
        for(int k=1;k<n;++k) {I c=original(path[k-1],path[k]);if(c<0) throw std::runtime_error("Internal error: negative path arc");total+=c;}
        return total;
    }
};
struct State {
    std::vector<int> path,rm,cm;
    std::vector<unsigned char> available;
    bool arcs=false;
    std::vector<int> forced_row,forced_col;
    std::vector<std::shared_ptr<const std::vector<int>>> banned;
    std::vector<Wide> u,v;
    std::vector<uint64_t> seen;
    I g=0,bound=0;
    int last=0;
    bool visited(int i) const {return (seen[i/64]>>(i%64))&1;}
};
// Bounded, four-way set-associative history. Evictions only lose pruning power.
// Full visited bitsets plus endpoint are compared; hashes are never proof evidence.
struct History {
    std::vector<uint64_t> slots;
    size_t capacity=0,width=0,words=0;
    uint64_t replacement=0;
    void configure(size_t bytes,size_t w) {
        words=w;width=w+2;
        size_t count=bytes/(width*sizeof(uint64_t));
        if(count>=4) capacity=std::bit_floor(count);
    }
    static uint64_t hash(const State& st) {
        uint64_t h=uint64_t(st.last)+0x9e3779b97f4a7c15ULL;
        for(auto x:st.seen) {x^=x>>30;x*=0xbf58476d1ce4e5b9ULL;x^=x>>27;x*=0x94d049bb133111ebULL;x^=x>>31;h^=x+0x9e3779b97f4a7c15ULL+(h<<6)+(h>>2);}
        return h;
    }
    bool dominated(const State& st,uint64_t h) {
        if(!capacity) return false;
        if(slots.empty()) slots.assign(capacity*width,~uint64_t(0));
        size_t base=(size_t(h)&(capacity-1))&~size_t(3),target=capacity;
        for(size_t k=0;k<4;++k) {
            size_t at=(base+k)*width;
            if(slots[at+words]==~uint64_t(0)) {if(target==capacity) target=base+k;continue;}
            if(slots[at+words]==uint64_t(st.last) && std::equal(st.seen.begin(),st.seen.end(),slots.begin()+long(at))) {
                if(slots[at+words+1]<=uint64_t(st.g)) return true;
                slots[at+words+1]=uint64_t(st.g);return false;
            }
        }
        if(target==capacity) target=base+size_t(replacement++&3);
        size_t at=target*width;std::copy(st.seen.begin(),st.seen.end(),slots.begin()+long(at));
        slots[at+words]=uint64_t(st.last);slots[at+words+1]=uint64_t(st.g);return false;
    }
};
struct alignas(64) HistoryShard {std::mutex mu;History table;};
struct CPU {int id=-1,node=-1,package=-1,core=-1;};
static int read_int(const std::filesystem::path& p) {std::ifstream f(p);int v=-1;f>>v;return v;}
static std::vector<CPU> cpus() {
    std::vector<CPU> result;
#ifdef __linux__
    cpu_set_t mask;CPU_ZERO(&mask);
    if(sched_getaffinity(0,sizeof(mask),&mask)==0) {
        std::map<int,std::vector<CPU>> nodes;
        std::set<std::pair<int,int>> cores;
        std::vector<CPU> siblings;
        for(int i=0;i<CPU_SETSIZE;++i) if(CPU_ISSET(i,&mask)) {
            auto p=std::filesystem::path("/sys/devices/system/cpu")/("cpu"+std::to_string(i));
            CPU c{i,-1,read_int(p/"topology/physical_package_id"),read_int(p/"topology/core_id")};
            std::error_code ec;
            for(auto it=std::filesystem::directory_iterator(p,ec);!ec && it!=std::filesystem::directory_iterator();it.increment(ec)) {
                auto name=it->path().filename().string();
                if(name.size()>4 && name.substr(0,4)=="node" && std::all_of(name.begin()+4,name.end(),[](char ch){return ch>='0'&&ch<='9';})) c.node=std::stoi(name.substr(4));
            }
            if(c.core<0 || cores.insert({c.package,c.core}).second) nodes[c.node].push_back(c);else siblings.push_back(c);
        }
        for(size_t k=0;;++k) {bool any=false;for(auto& [node,list]:nodes) if(k<list.size()) {result.push_back(list[k]);any=true;} if(!any) break;}
        result.insert(result.end(),siblings.begin(),siblings.end());
    }
#endif
    if(result.empty()) result.push_back(CPU{});
    return result;
}
struct alignas(64) Queue {std::mutex mu;std::deque<State> tasks;};
struct Solver;
struct Worker {
    Solver& s;int id;CPU cpu;int engine=0;
    std::vector<I> c;
    std::vector<int> columns,way;
    std::vector<Wide> dist;
    std::vector<unsigned char> used;
    History history;
    uint64_t nodes=0,dominance=0,bounds=0;
    std::mt19937_64 rng;
    Worker(Solver& solver,int index,CPU location);
    I edge(int i,int j) const;
    I edge(const State& st,int i,int j) const;
    void available(State& st);
    void check();
    std::vector<State> arc_stack;
    void arc_dfs(State& st);
    void arc_expand(State& st);
    bool augment(State& st,int row);
    bool root(State& st);
    bool child(const State& parent,int x,State& out);
    void dfs(State& st);
    void assignment_solution(const State& st);
    void run();
};
struct Solver {
    Problem& p;Control& ctl;const Options& opt;
    std::atomic<I> best{INF};
    std::mutex best_mu,error_mu;
    std::vector<int> best_path;
    std::vector<std::unique_ptr<Queue>> queues;
    std::vector<std::unique_ptr<Worker>> workers;
    std::atomic<uint64_t> outstanding[2]{{0},{0}};
    std::atomic<bool> finished{false};
    std::atomic<int> winner{-1};
    std::exception_ptr error;
    std::vector<std::unique_ptr<HistoryShard>> shared_history;
    I root_bound=0;
    bool proven=false;
    Solver(Problem& pp,Control& cc,const Options& oo):p(pp),ctl(cc),opt(oo) {
        if(opt.shared_history && opt.history_mb) for(int i=0;i<256;++i) {
            auto shard=std::make_unique<HistoryShard>();
            shard->table.configure(opt.history_mb*1024*1024/256,size_t(p.words));
            shared_history.push_back(std::move(shard));
        }
    }
    bool dominated(const State& st,History& local) {
        if(!opt.history_mb) return false;
        uint64_t h=History::hash(st);
        if(!opt.shared_history) return local.dominated(st,h);
        auto& shard=*shared_history[size_t(h>>56)];
        std::lock_guard lock(shard.mu);
        return shard.table.dominated(st,h);
    }
    void offer(const std::vector<int>& path) {
        I value=p.validate(path);
        if(value>=best.load(std::memory_order_relaxed)) return;
        std::lock_guard lock(best_mu);
        if(value<best.load()) {best_path=path;best.store(value);}
    }
    std::vector<int> greedy(std::mt19937_64& rng,int variant) {
        std::vector<int> degree(p.n),ready,path;
        path.reserve(p.n);
        for(int i=0;i<p.n;++i) {degree[i]=int(p.pred[i].size());if(!degree[i]) ready.push_back(i);}
        int last=p.n;
        while(!ready.empty()) {
            ctl.check();
            std::sort(ready.begin(),ready.end(),[&](int a,int b){return std::pair{p.original(last,a),a}<std::pair{p.original(last,b),b};});
            size_t k=0;
            if(variant && ready.size()>1 && rng()%100<25) k=size_t(rng()%std::min<size_t>(ready.size(),4));
            int x=ready[k];ready.erase(ready.begin()+long(k));path.push_back(x);last=x;
            for(int y:p.succ[x]) if(--degree[y]==0) ready.push_back(y);
        }
        return path;
    }
    void improve(std::vector<int>& path,double until,int max_passes=16) {
        int n=p.n;
        for(int pass=0;pass<max_passes;++pass) {
            if(finished.load() || ctl.expired() || ctl.elapsed()>=until) return;
            std::vector<int> pos(n);for(int k=0;k<n;++k) pos[path[k]]=k;
            I best_delta=0;int ba=-1,bk=0,bj=0;
            auto at=[&](int i){return i<0 || i>=n?p.n:path[i];};
            for(int a=0;a<n;++a) {
                if(finished.load() || ctl.expired() || ctl.elapsed()>=until) return;
                for(int len=1;len<=3 && a+len<=n;++len) {
                    int lo=0,hi=n;
                    for(int k=a;k<a+len;++k) {
                        for(int q:p.pred[path[k]]) if(pos[q]<a || pos[q]>=a+len) lo=std::max(lo,pos[q]+1);
                        for(int q:p.succ[path[k]]) if(pos[q]<a || pos[q]>=a+len) hi=std::min(hi,pos[q]);
                    }
                    int first=path[a],last=path[a+len-1],prev=at(a-1),next=at(a+len);
                    I remove=p.original(prev,next)-p.original(prev,first)-p.original(last,next);
                    for(int j=lo;j<=hi;++j) {
                        if(j>=a && j<=a+len) continue;
                        int l=at(j-1),r=at(j);
                        if(p.original(l,first)<0 || p.original(last,r)<0 || p.original(prev,next)<0) continue;
                        I delta=remove+p.original(l,first)+p.original(last,r)-p.original(l,r);
                        if(delta<best_delta) {best_delta=delta;ba=a;bk=len;bj=j;}
                    }
                }
            }
            if(ba<0) return;
            std::vector<int> block(path.begin()+ba,path.begin()+ba+bk);
            path.erase(path.begin()+ba,path.begin()+ba+bk);
            if(bj>ba) bj-=bk;
            path.insert(path.begin()+bj,block.begin(),block.end());
            offer(path);
        }
    }
    void heuristic(std::mt19937_64& rng,double seconds) {
        double until=std::min(ctl.limit,ctl.elapsed()+seconds);
        for(int attempt=0;attempt<32 && ctl.elapsed()<until;++attempt) {
            ctl.check();if(finished.load()) return;
            std::vector<int> path;
            if(attempt%8==7) path=greedy(rng,1);
            else {
                {std::lock_guard lock(best_mu);path=best_path;}
                // Random feasible relocations perturb the incumbent; local search repairs it.
                int moves=1+int(rng()%4);
                for(int move=0;move<moves;++move) {
                    int a=int(rng()%p.n),x=path[a],lo=0,hi=p.n;
                    std::vector<int> pos(p.n);for(int k=0;k<p.n;++k) pos[path[k]]=k;
                    for(int q:p.pred[x]) lo=std::max(lo,pos[q]+1);
                    for(int q:p.succ[x]) hi=std::min(hi,pos[q]);
                    int j=lo+int(rng()%uint64_t(hi-lo+1));
                    if(j==a || j==a+1) continue;
                    path.erase(path.begin()+a);if(j>a) --j;path.insert(path.begin()+j,x);
                }
            }
            offer(path);improve(path,until,24);
        }
    }
    bool pop(int id,State& st) {
        {std::lock_guard lock(queues[id]->mu);if(!queues[id]->tasks.empty()) {st=std::move(queues[id]->tasks.back());queues[id]->tasks.pop_back();return true;}}
        for(int pass=0;pass<2;++pass) for(size_t k=1;k<workers.size();++k) {
            int victim=(id+int(k))%int(workers.size());
            if(workers[id]->engine!=workers[victim]->engine) continue;
            bool local=workers[id]->cpu.node==workers[victim]->cpu.node;
            if(local!=(pass==0)) continue;
            std::unique_lock lock(queues[victim]->mu,std::try_to_lock);
            if(lock && !queues[victim]->tasks.empty()) {st=std::move(queues[victim]->tasks.front());queues[victim]->tasks.pop_front();return true;}
        }
        return false;
    }
    bool offload(int id,State& st) {
        if(opt.threads==1 || outstanding[workers[id]->engine].load(std::memory_order_relaxed)>=uint64_t(opt.threads)*4) return false;
        std::lock_guard lock(queues[id]->mu);
        if(queues[id]->tasks.size()>=2) return false;
        outstanding[workers[id]->engine].fetch_add(1);queues[id]->tasks.push_back(std::move(st));return true;
    }
    void solve();
};
Worker::Worker(Solver& solver,int index,CPU location):s(solver),id(index),cpu(location),rng(s.opt.seed+uint64_t(index)*0x9e3779b97f4a7c15ULL) {
    history.configure(s.opt.history_mb*1024*1024/size_t(s.opt.threads),size_t(s.p.words));
}
I Worker::edge(int i,int j) const {return c[size_t(i)*s.p.m+j];}
void Worker::check() {if(s.finished.load(std::memory_order_relaxed)) throw SearchDone{};s.ctl.check();}
I Worker::edge(const State& st,int i,int j) const {
    if(st.arcs) {
        if((st.forced_row[i]>=0 && st.forced_row[i]!=j) || (st.forced_col[j]>=0 && st.forced_col[j]!=i)) return INF;
        const auto& ban=st.banned[i];
        if(ban && std::binary_search(ban->begin(),ban->end(),j)) return INF;
    } else if(i==st.last && !st.available[j]) return INF;
    return edge(i,j);
}
void Worker::available(State& st) {
    st.available.assign(s.p.m,0);
    for(int x=0;x<s.p.n;++x) if(!st.visited(x)) {
        bool okay=true;for(int pred:s.p.pred[x]) if(!st.visited(pred)) {okay=false;break;}
        st.available[x]=okay;
    }
    st.available[s.p.n]=int(st.path.size())==s.p.n;
}
// One Hungarian shortest augmenting path. All arithmetic and comparisons exact.
bool Worker::augment(State& st,int row) {
    int m=s.p.m,virtual_col=m;
    dist.assign(m,WINF);way.assign(m,-1);used.assign(m+1,0);
    int j0=virtual_col;
    do {
        check();used[j0]=1;
        int i0=j0==virtual_col?row:st.cm[j0],j1=-1;Wide delta=WINF;
        for(int j:columns) if(!used[j]) {
            I cost=edge(st,i0,j);
            if(cost<INF) {Wide cur=cost-st.u[i0]-st.v[j];if(cur<dist[j]) {dist[j]=cur;way[j]=j0;}}
            if(dist[j]<delta) {delta=dist[j];j1=j;}
        }
        if(j1<0) return false;
        st.u[row]+=delta;
        for(int j:columns) {
            if(used[j]) {st.u[st.cm[j]]+=delta;st.v[j]-=delta;}
            else if(dist[j]<WINF) dist[j]-=delta;
        }
        j0=j1;
    } while(st.cm[j0]>=0);
    do {
        int previous=way[j0];int r=previous==virtual_col?row:st.cm[previous];
        st.cm[j0]=r;st.rm[r]=j0;j0=previous;
    } while(j0!=virtual_col);
    return true;
}
bool Worker::root(State& st) {
    check();++nodes;
    int m=s.p.m;st.last=s.p.n;st.seen.assign(s.p.words,0);st.rm.assign(m,-1);st.cm.assign(m,-1);st.u.assign(m,0);st.v.assign(m,0);
    available(st);
    columns.resize(m);std::iota(columns.begin(),columns.end(),0);
    for(int i=0;i<m;++i) {
        I minimum=INF;for(int j=0;j<m;++j) minimum=std::min(minimum,edge(st,i,j));
        if(minimum==INF) return false;
        st.u[i]=minimum;
    }
    for(int i=0;i<m;++i) if(!augment(st,i)) return false;
    for(int i=0;i<m;++i) st.bound+=edge(i,st.rm[i]);
    return true;
}
bool Worker::child(const State& parent,int x,State& out) {
    check();++nodes;
    out=parent;
    int r=parent.last,a=out.rm[r],b=out.cm[x];
    out.g+=edge(r,x);out.last=x;out.path.push_back(x);out.seen[x/64]|=uint64_t(1)<<(x%64);
    out.rm[r]=-1;out.cm[a]=-1;out.cm[x]=-1;
    if(b!=r) out.rm[b]=-1;
    available(out);
    // Restrict the new prefix endpoint to precedence-eligible successors.
    if(out.rm[x]>=0 && !out.available[out.rm[x]]) {out.cm[out.rm[x]]=-1;out.rm[x]=-1;}
    columns.clear();
    for(int j=0;j<s.p.n;++j) if(!out.visited(j)) columns.push_back(j);
    columns.push_back(s.p.n);
    if(b!=r && out.rm[b]<0 && !augment(out,b)) return false;
    if(out.rm[x]<0 && !augment(out,x)) return false;
    out.bound=out.g;
    for(int i=0;i<s.p.m;++i) if(out.rm[i]>=0) out.bound+=edge(i,out.rm[i]);
    return true;
}
void Worker::assignment_solution(const State& st) {
    std::vector<int> path=st.path;
    std::vector<unsigned char> seen(s.p.n,0);for(int x:path) seen[x]=1;
    int cur=st.last;
    for(int k=int(path.size());k<s.p.n;++k) {
        int next=st.rm[cur];if(next<0 || next==s.p.n || seen[next]) return;
        path.push_back(next);seen[next]=1;cur=next;
    }
    if(st.rm[cur]!=s.p.n) return;
    std::vector<int> pos(s.p.n);for(int k=0;k<s.p.n;++k) pos[path[k]]=k;
    for(int i=0;i<s.p.n;++i) for(int j:s.p.pred[i]) if(pos[j]>=pos[i]) return;
    s.offer(path);
}
void Worker::dfs(State& st) {
    check();
    if(st.bound>=s.best.load(std::memory_order_relaxed)) {++bounds;return;}
    if(int(st.path.size())==s.p.n) {s.offer(st.path);return;}
    if(s.dominated(st,history)) {++dominance;return;}
    assignment_solution(st);
    if(st.bound>=s.best.load(std::memory_order_relaxed)) return;
    if((nodes&8191)==0) {
        s.heuristic(rng,0.01);
    }
    struct Candidate {int x;Wide estimate;I cost;};std::vector<Candidate> cand;
    for(int x=0;x<s.p.n;++x) if(!st.visited(x) && edge(st.last,x)<INF) {
        bool eligible=true;
        for(int pred:s.p.pred[x]) if(!st.visited(pred)) {eligible=false;break;}
        if(!eligible) continue;
        Wide estimate=st.bound+edge(st.last,x)-st.u[st.last]-st.v[x];
        if(estimate<s.best.load(std::memory_order_relaxed)) cand.push_back({x,estimate,edge(st.last,x)});
    }
    std::sort(cand.begin(),cand.end(),[](const Candidate& a,const Candidate& b){return std::tie(a.estimate,a.cost,a.x)<std::tie(b.estimate,b.cost,b.x);});
    for(size_t k=0;k<cand.size();++k) {
        check();if(cand[k].estimate>=s.best.load(std::memory_order_relaxed)) continue;
        State next;
        if(!child(st,cand[k].x,next)) continue;
        if(next.bound>=s.best.load(std::memory_order_relaxed)) {++bounds;continue;}
        // Keep one branch on this worker; expose siblings for work stealing.
        if(k+1<cand.size() && s.offload(id,next)) continue;
        dfs(next);
    }
}
// Disjoint include/exclude partition of an assignment subtour or precedence conflict.
void Worker::arc_dfs(State& st) {
    arc_stack.push_back(std::move(st));
    while(!arc_stack.empty()) {
        check();State current=std::move(arc_stack.back());arc_stack.pop_back();arc_expand(current);
    }
}
void Worker::arc_expand(State& st) {
    check();
    if(st.bound>=s.best.load(std::memory_order_relaxed)) {++bounds;return;}
    if((nodes&8191)==0) s.heuristic(rng,0.01);
    int m=s.p.m,n=s.p.n;
    std::vector<unsigned char> seen(m,0);
    std::vector<std::pair<int,int>> conflict;
    std::vector<int> main_path;
    for(int start=0;start<m;++start) if(!seen[start]) {
        std::vector<int> cycle;int x=start;bool dummy=false;
        do {seen[x]=1;cycle.push_back(x);dummy|=x==n;x=st.rm[x];} while(x!=start);
        if(!dummy && (conflict.empty() || cycle.size()<conflict.size())) {
            conflict.clear();for(int i:cycle) conflict.emplace_back(i,st.rm[i]);
        }
    }
    if(conflict.empty()) {
        int x=st.rm[n];while(x!=n) {main_path.push_back(x);x=st.rm[x];}
        std::vector<int> pos(n);for(int k=0;k<n;++k) pos[main_path[k]]=k;
        int begin=-1,end=n+1;
        for(int i=0;i<n;++i) for(int j:s.p.pred[i]) if(pos[i]<pos[j] && pos[j]-pos[i]<end-begin) {begin=pos[i];end=pos[j];}
        if(begin<0) {s.offer(main_path);return;}
        for(int k=begin;k<end;++k) conflict.emplace_back(main_path[k],main_path[k+1]);
    }
    // Already forced edges cannot be excluded. At least one remaining edge must go.
    conflict.erase(std::remove_if(conflict.begin(),conflict.end(),[&](auto e){return st.forced_row[e.first]==e.second;}),conflict.end());
    if(conflict.empty()) return;
    // Branch first on arcs with inexpensive replacement options.
    auto penalty=[&](std::pair<int,int> e) {
        Wide row=WINF,col=WINF;
        for(int k=0;k<m;++k) {
            I a=edge(st,e.first,k),b=edge(st,k,e.second);
            if(k!=e.second && a<INF) row=std::min(row,a-st.u[e.first]-st.v[k]);
            if(k!=e.first && b<INF) col=std::min(col,b-st.u[k]-st.v[e.second]);
        }
        return row==WINF || col==WINF ? WINF : row+col;
    };
    std::vector<std::pair<Wide,std::pair<int,int>>> ordered;
    for(auto e:conflict) ordered.push_back({penalty(e),e});
    std::sort(ordered.begin(),ordered.end());
    State included=st;
    for(size_t k=0;k<ordered.size();++k) {
        check();if(st.bound>=s.best.load(std::memory_order_relaxed)) return;
        auto [i,j]=ordered[k].second;
        ++nodes;
        State next=included;
        auto ban=std::make_shared<std::vector<int>>(next.banned[i]?*next.banned[i]:std::vector<int>{});
        ban->insert(std::lower_bound(ban->begin(),ban->end(),j),j);next.banned[i]=ban;
        next.rm[i]=-1;next.cm[j]=-1;
        columns.resize(m);std::iota(columns.begin(),columns.end(),0);
        if(augment(next,i)) {
            next.bound=0;for(int r=0;r<m;++r) next.bound+=edge(r,next.rm[r]);
            if(next.bound<s.best.load(std::memory_order_relaxed)) {
                if(k+1==ordered.size() || !s.offload(id,next)) arc_stack.push_back(std::move(next));
            } else ++bounds;
        }
        // Inclusion preserves the current optimal assignment and its duals.
        included.forced_row[i]=j;included.forced_col[j]=i;
    }
}
void Worker::run() {
    try {
#ifdef __linux__
        if(s.opt.pin && cpu.id>=0) {cpu_set_t mask;CPU_ZERO(&mask);CPU_SET(cpu.id,&mask);if(pthread_setaffinity_np(pthread_self(),sizeof(mask),&mask)!=0) {std::lock_guard lock(s.error_mu);std::cerr<<"Warning: could not pin worker "<<id<<"\n";}}
#endif
        // Allocate and first-touch private matrix/scratch after affinity is set.
        c=s.p.cost;
        s.heuristic(rng,0.15);
        while(!s.ctl.expired() && !s.finished.load()) {
            if(s.outstanding[engine].load()==0) {int none=-1;s.winner.compare_exchange_strong(none,engine);s.finished.store(true);break;}
            State st;
            if(s.pop(id,st)) {if(engine==0) dfs(st);else arc_dfs(st);if(s.outstanding[engine].fetch_sub(1)==1) {int none=-1;s.winner.compare_exchange_strong(none,engine);s.finished.store(true);}}
            else std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    } catch(const SearchDone&) {} catch(const TimedOut&) {} catch(...) {std::lock_guard lock(s.error_mu);if(!s.error) s.error=std::current_exception();s.ctl.stop.store(true);}
}
void Solver::solve() {
    if(p.cyclic) {std::cerr<<"Infeasible: precedence graph contains a cycle.\n";return;}
    std::mt19937_64 rng(opt.seed);
    // Establish a feasible incumbent before potentially costly preprocessing.
    auto path=greedy(rng,0);offer(path);
    if(p.n==1 || p.unique_order || best.load()==0) {proven=!ctl.expired();return;}
    p.preprocess(ctl);
    improve(path,std::min(ctl.elapsed()+0.1,std::isfinite(ctl.limit)?ctl.limit*0.05:ctl.elapsed()+0.1));
    auto locations=cpus();
    for(int i=0;i<opt.threads;++i) {queues.push_back(std::make_unique<Queue>());workers.push_back(std::make_unique<Worker>(*this,i,locations[size_t(i)%locations.size()]));}
    Worker& w=*workers[0];w.c=p.cost;
    State initial;
    if(!w.root(initial)) throw std::runtime_error("Internal error: acyclic SOP has no assignment");
    root_bound=initial.bound;w.assignment_solution(initial);
    std::cerr<<"Root lower bound: "<<root_bound<<"; incumbent: "<<best.load()<<"; workers: "<<opt.threads<<"\n";
    if(initial.bound>=best.load()) {proven=!ctl.expired();return;}
    // A one-thread portfolio selects arcs; two or more threads run both engines.
    bool prefix=opt.strategy=="prefix" || (opt.strategy=="portfolio" && opt.threads>1);
    bool arcs=opt.strategy!="prefix";
    int split=prefix && arcs ? (opt.threads+1)/2 : (prefix?opt.threads:0);
    for(int i=0;i<opt.threads;++i) workers[i]->engine=i<split?0:1;
    if(prefix) {queues[0]->tasks.push_back(initial);outstanding[0].store(1);}
    if(arcs) {
        initial.arcs=true;initial.forced_row.assign(p.m,-1);initial.forced_col.assign(p.m,-1);initial.banned.resize(p.m);
        queues[split]->tasks.push_back(std::move(initial));outstanding[1].store(1);
    }
    // Drop coordinator's matrix so worker 0 can first-touch its own allocation.
    std::vector<I>().swap(w.c);
    std::vector<std::thread> threads;
    try {for(auto& worker:workers) threads.emplace_back([ptr=worker.get()]{ptr->run();});}
    catch(...) {ctl.stop.store(true);for(auto& t:threads) t.join();throw;}
    for(auto& t:threads) t.join();
    if(error) std::rethrow_exception(error);
    proven=finished.load() && !ctl.expired();
    if(proven) std::cerr<<"Proof completed by "<<(winner.load()==0?"prefix":"arc")<<" engine.\n";
}
static std::string csv_quote(const std::string& v) {std::string out="\"";for(char ch:v) {if(ch=='\"') out+='\"';out+=ch;}return out+'\"';}
static void output(const Options& opt,int n,double elapsed,uint64_t nodes,I cost,const std::vector<int>& path,bool proven) {
    const std::string header="Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal\n";
    std::ostringstream row;row<<csv_quote(std::filesystem::path(opt.input).filename().string())<<','<<n<<','<<std::fixed<<std::setprecision(6)<<elapsed<<','<<nodes<<','<<cost<<",\"";
    for(size_t i=0;i<path.size();++i) {if(i) row<<' ';row<<path[i];}row<<"\","<<(proven?"yes":"no")<<'\n';
    if(!opt.output.empty()) {
        bool need_header=!std::filesystem::exists(opt.output) || std::filesystem::file_size(opt.output)==0;
        std::ofstream f(opt.output,std::ios::app);if(!f) throw std::runtime_error("Cannot open output: "+opt.output);
        if(opt.header && need_header) f<<header;
        f<<row.str();f.flush();if(!f) throw std::runtime_error("Failed writing output CSV");
    }
    if(opt.header) std::cout<<header;
    std::cout<<row.str();std::cout.flush();if(!std::cout) throw std::runtime_error("Failed writing stdout");
}
int main(int argc,char** argv) {
    Control ctl;
    std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
    try {
        Options opt=options(argc,argv,ctl);Problem p;
        try {p.read(opt.input,ctl);} catch(const TimedOut&) {output(opt,p.n,ctl.elapsed(),0,-1,{},false);return 0;}
        Solver solver(p,ctl,opt);
        try {solver.solve();} catch(const TimedOut&) {}
        uint64_t nodes=0,dom=0,bounds=0;
        for(auto& w:solver.workers) {nodes+=w->nodes;dom+=w->dominance;bounds+=w->bounds;}
        I cost=-1;
        if(!solver.best_path.empty()) {cost=p.validate(solver.best_path);if(cost!=solver.best.load()) throw std::runtime_error("Internal error: incumbent cost mismatch");}
        bool proven=solver.proven && !ctl.expired() && cost>=0;
        std::cerr<<"Processed states: "<<nodes<<"; history prunes: "<<dom<<"; bound prunes: "<<bounds<<"; "<<(proven?"optimality proven":(ctl.stop.load()?"interrupted/time limit":"no optimality claim"))<<"\n";
        output(opt,p.n,ctl.elapsed(),nodes,cost,solver.best_path,proven);
        return 0;
    } catch(const std::exception& e) {std::cerr<<"Error: "<<e.what()<<'\n';return 2;}
}

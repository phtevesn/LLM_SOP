// Standalone SOP solver. C++17, standard library only.
#include <algorithm>
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
struct Key {
    std::uint64_t mask; int last;
    bool operator==(const Key& b) const { return mask==b.mask && last==b.last; }
};
struct Hash { size_t operator()(const Key& k) const {
    return std::hash<std::uint64_t>{}(k.mask ^ (std::uint64_t(k.last)*0x9e3779b97f4a7c15ULL));
}};

class Solver {
    const Input& in;
    int m, start, finish, words;
    std::vector<std::vector<Cost>> c;
    std::vector<std::vector<int>> succ;
    std::vector<std::vector<std::uint64_t>> reach;
    std::vector<int> degree, path;
    std::vector<char> used;
    std::unordered_map<Key,Cost,Hash> history;
    Clock::time_point deadline;
    std::mt19937 rng{20261012};
    bool precedes(int a,int b) const { return (reach[a][b/64]>>(b%64))&1; }
    bool expired() const { return Clock::now()>=deadline; }
    bool allowed(int a,int b) const { return a!=b && c[a][b]>=0 && !precedes(b,a); }
    Cost cost(const std::vector<int>& p) const {
        Cost s=0; for(size_t i=1;i<p.size();i++) s+=c[p[i-1]][p[i]]; return s;
    }
    void accept(const std::vector<int>& p) {
        Cost z=cost(p); if(z<best) {best=z; best_path=p;}
    }
    std::vector<int> greedy(bool randomize) {
        auto d=degree; std::vector<char> taken(m); std::vector<int> p;
        for(int step=0;step<m;step++) {
            std::vector<int> choices;
            for(int v=0;v<m;v++) if(!taken[v] && d[v]==0) choices.push_back(v);
            if(choices.empty()) throw std::runtime_error("Cyclic precedence constraints");
            int last=p.empty()?start:p.back();
            std::sort(choices.begin(),choices.end(),[&](int a,int b){
                return c[last][a]!=c[last][b] ? c[last][a]<c[last][b] : a<b;
            });
            int v=choices[0];
            if(randomize) v=choices[std::uniform_int_distribution<int>(0,std::min(3,int(choices.size())-1))(rng)];
            p.push_back(v); taken[v]=1; for(int w:succ[v]) --d[w];
        }
        return p;
    }
    // Relocate one original vertex without crossing any predecessor/successor.
    // Delta evaluation is O(1); one sweep is O(N^2).
    void improve(std::vector<int>& p, Clock::time_point until) {
        bool changed=true;
        while(changed && Clock::now()<until) {
            changed=false;
            for(int i=1;i<m-1;i++) {
                if(Clock::now()>=until) return;
                int x=p[i], a=p[i-1], b=p[i+1];
                Cost removal=c[a][b]-c[a][x]-c[x][b];
                Cost delta=0; int where=-1;
                for(int j=i-2;j>=0;j--) {
                    if(precedes(p[j+1],x)) break;
                    int u=p[j],v=p[j+1];
                    Cost d=removal+c[u][x]+c[x][v]-c[u][v];
                    if(d<delta) {delta=d;where=j;}
                }
                for(int j=i+1;j<m-1;j++) {
                    if(precedes(x,p[j])) break;
                    int u=p[j],v=p[j+1];
                    Cost d=removal+c[u][x]+c[x][v]-c[u][v];
                    if(d<delta) {delta=d;where=j;}
                }
                if(where>=0) {
                    p.erase(p.begin()+i);
                    p.insert(p.begin()+(where<i?where+1:where),x);
                    changed=true;
                }
            }
        }
    }
    // Every completion chooses one outgoing arc for last and each unvisited
    // nonterminal, and one incoming arc for each unvisited vertex. Independent
    // row/column minima relax uniqueness and subtours, hence are lower bounds.
    Cost lower_bound(int last) const {
        std::vector<int> rows{last}, cols;
        for(int v=0;v<m;v++) if(!used[v]) {
            cols.push_back(v); if(v!=finish) rows.push_back(v);
        }
        std::vector<Cost> column(cols.size(),INF); Cost out=0;
        for(int a:rows) {
            Cost minimum=INF;
            for(size_t k=0;k<cols.size();k++) {
                int b=cols[k];
                if(allowed(a,b)) {minimum=std::min(minimum,c[a][b]);column[k]=std::min(column[k],c[a][b]);}
            }
            if(minimum==INF) return INF;
            out+=minimum;
        }
        Cost incoming=0;
        for(Cost z:column) {if(z==INF) return INF;incoming+=z;}
        return std::max(out,incoming);
    }
    void dfs(int last,Cost prefix,std::uint64_t mask) {
        if(expired()) {timed_out=true;return;}
        ++nodes;
        if(prefix>=best) return; // All valid costs are nonnegative.
        if(int(path.size())==m) { best=prefix;best_path=path;return; }
        if(m<=64) {
            Key k{mask,last}; auto it=history.find(k);
            if(it!=history.end() && it->second<=prefix) return;
            // Clearing affects performance only, never correctness.
            if(history.size()>=500000) history.clear();
            history[k]=prefix;
        }
        Cost lb=lower_bound(last);
        if(lb==INF || prefix+lb>=best) return;
        std::vector<int> choices;
        for(int v=0;v<m;v++) if(!used[v] && degree[v]==0) choices.push_back(v);
        std::sort(choices.begin(),choices.end(),[&](int a,int b){
            return c[last][a]!=c[last][b] ? c[last][a]<c[last][b] : a<b;
        });
        for(int v:choices) {
            used[v]=1;path.push_back(v);for(int w:succ[v]) --degree[w];
            dfs(v,prefix+c[last][v],m<=64 ? mask|(std::uint64_t(1)<<v) : 0);
            for(int w:succ[v]) ++degree[w];
            path.pop_back();used[v]=0;
            if(timed_out) return;
        }
    }
public:
    Cost best=INF;
    std::vector<int> best_path;
    std::uint64_t nodes=0;
    bool timed_out=false, optimal=false;
    Solver(const Input& input,Clock::time_point end):in(input),m(in.n+2),start(in.n),finish(in.n+1),
        words((m+63)/64),c(m,std::vector<Cost>(m)),succ(m),reach(m,std::vector<std::uint64_t>(words)),
        degree(m),used(m),deadline(end) {
        auto edge=[&](int a,int b) {succ[a].push_back(b);++degree[b];reach[a][b/64]|=std::uint64_t(1)<<(b%64);};
        for(int i=0;i<in.n;i++) for(int j=0;j<in.n;j++) {
            c[i][j]=in.t[i][j];if(i!=j && in.t[i][j]<0) edge(j,i);
        }
        // Zero-cost virtual endpoints avoid assumptions about matrix ordering.
        // Unique original DAG source/sink become the first/last original nodes.
        for(int v=0;v<in.n;v++) {edge(start,v);edge(v,finish);}
        for(int k=0;k<m;k++) for(int i=0;i<m;i++) if(precedes(i,k))
            for(int w=0;w<words;w++) reach[i][w]|=reach[k][w];
        for(int v=0;v<m;v++) if(precedes(v,v)) throw std::runtime_error("Cyclic precedence constraints: infeasible");
    }
    void solve(double limit) {
        auto p=greedy(false);accept(p); // Guaranteed feasible for an acyclic input.
        // At most 20% of requested time (capped at 2s) on incumbent search.
        auto until=std::min(deadline,Clock::now()+std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(std::min(2.0,limit*0.2))));
        improve(p,until);accept(p);
        while(Clock::now()<until) {p=greedy(true);improve(p,until);accept(p);}
        used[start]=1;path={start};for(int w:succ[start]) --degree[w];
        dfs(start,0,m<=64 ? std::uint64_t(1)<<start : 0);
        optimal=!timed_out;
    }
    // Independently validate against the ORIGINAL matrix, not search state.
    std::vector<int> validated_path() const {
        std::vector<int> p(best_path.begin()+1,best_path.end()-1),position(in.n,-1);
        if(int(p.size())!=in.n) throw std::runtime_error("Internal validation: wrong path length");
        for(int k=0;k<in.n;k++) {
            int v=p[k];if(v<0 || v>=in.n || position[v]>=0) throw std::runtime_error("Internal validation: not a permutation");
            position[v]=k;
        }
        for(int i=0;i<in.n;i++) for(int j=0;j<in.n;j++)
            if(i!=j && in.t[i][j]<0 && position[j]>=position[i]) throw std::runtime_error("Internal validation: precedence violation");
        Cost sum=0;
        for(int k=1;k<in.n;k++) {
            Cost z=in.t[p[k-1]][p[k]];if(z<0) throw std::runtime_error("Internal validation: negative arc");sum+=z;
        }
        if(sum!=best) throw std::runtime_error("Internal validation: cost mismatch");
        return p;
    }
};
std::string csv(std::string s) {
    std::string r="\"";for(char c:s) {r+=c;if(c=='"') r+='"';} return r+'"';
}
int main(int argc,char** argv) {
    try {
        double limit=60;std::string output;std::vector<std::string> files;bool positional=false;
        for(int i=1;i<argc;i++) {
            std::string a=argv[i];
            if(!positional && a=="--") {positional=true;continue;}
            if(!positional && (a=="--help" || a=="-h")) {
                std::cout<<"Usage: sop_solver [--time-limit SECONDS] [--output results.csv] FILE.sop ...\n"
                           "Default: 60 seconds per file. CSV on stdout; status on stderr.\n";return 0;
            }
            if(!positional && (a=="--time-limit" || a=="-t" || a=="--output" || a=="-o")) {
                if(++i==argc) throw std::runtime_error("Missing option value");
                if(a=="-o" || a=="--output") output=argv[i];
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
                Solver solver(input,deadline);solver.solve(limit);auto p=solver.validated_path();
                double elapsed=std::chrono::duration<double>(Clock::now()-begin).count();
                std::ostringstream path;for(size_t i=0;i<p.size();i++) {if(i) path<<' ';path<<p[i];}
                *out<<csv(std::filesystem::path(file).filename().string())<<','<<n<<','<<std::fixed<<std::setprecision(6)
                    <<elapsed<<','<<solver.nodes<<','<<solver.best<<','<<csv(path.str())<<'\n';
                std::cerr<<file<<": "<<(solver.optimal?"OPTIMAL (proven)":"TIME_LIMIT (best found; not proven optimal)")
                         <<"; cost="<<solver.best<<"; start="<<p.front()<<"; end="<<p.back()<<'\n';
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

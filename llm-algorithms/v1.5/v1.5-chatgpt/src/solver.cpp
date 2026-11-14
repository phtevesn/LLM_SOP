// Exact parallel SOP search: prefix branch-and-bound, incremental assignment,
// exact-key dominance, feasible matching completions, and small-suffix dynamic programming.
#include "sop/solver.hpp"
#include "sop/heuristics.hpp"
#include "sop/history.hpp"
#include "sop/parallel.hpp"
#include "sop/tail_dp.hpp"
#include "sop/dominance.hpp"

#include <algorithm>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace sop {
namespace {
struct alignas(64) Statistics {
    std::uint64_t nodes = 0, bound_prunes = 0, history_prunes = 0, exchange_prunes = 0, tasks = 0, dp_calls = 0;
};

class Worker {
public:
    Worker(int id, const Graph& graph, const Options& options, Control& control,
           Incumbent& incumbent, Scheduler& scheduler, SharedHistory* shared,
           std::size_t local_bytes, Cost root_bound, Statistics& stats)
        : id_(id), graph_(graph), options_(options), control_(control), incumbent_(incumbent),
          scheduler_(scheduler), shared_(shared), local_(graph.words, local_bytes),
          assignment_(graph_, control), root_bound_(root_bound), stats_(stats),
          visited_(graph.words, 0), degree_(graph.n, 0), rng_(mix64(options.seed + static_cast<unsigned>(id))) {}
    void run(const AssignmentState& root, Clock::time_point heuristic_end) {
        heuristic_portfolio(graph_, incumbent_, control_, mix64(options_.seed + static_cast<unsigned>(id_)), heuristic_end, root);
        Task task;
        while (scheduler_.take(id_, task)) {
            ++stats_.tasks;
            assignment_.load(task.assignment); path_ = std::move(task.prefix);
            std::fill(visited_.begin(), visited_.end(), 0); hash_ = 0;
            for (int v : path_) { set_bit(visited_, v); hash_ ^= graph_.zobrist[v]; }
            for (int v = 0; v < graph_.n; ++v) {
                degree_[v] = 0;
                for (int p : graph_.predecessors[v]) if (!bit(visited_, p)) ++degree_[v];
            }
            visit(task.cost);
            scheduler_.finish();
        }
    }
private:
    int id_;
    Graph graph_; // Worker-local copy: allocated and first-touched after CPU binding.
    const Options& options_;
    Control& control_;
    Incumbent& incumbent_;
    Scheduler& scheduler_;
    SharedHistory* shared_;
    HistoryTable local_;
    Assignment assignment_;
    Cost root_bound_;
    Statistics& stats_;
    std::vector<int> path_;
    std::vector<std::uint64_t> visited_;
    std::vector<int> degree_;
    std::uint64_t hash_ = 0;
    std::mt19937_64 rng_;
    struct Candidate { int vertex; Cost bound, edge; bool donated = false; };

    void push(int v) {
        path_.push_back(v); set_bit(visited_, v); hash_ ^= graph_.zobrist[v];
        for (int s : graph_.successors[v]) --degree_[s];
    }
    void pop(int v) {
        for (int s : graph_.successors[v]) ++degree_[s];
        hash_ ^= graph_.zobrist[v]; clear_bit(visited_, v); path_.pop_back();
    }
    void branch(Candidate& candidate, Cost cost, bool donate) {
        if (candidate.bound >= incumbent_.cost() || control_.stopped()) return;
        const int last = path_.empty() ? graph_.n : path_.back(), next = candidate.vertex;
        const auto checkpoint = assignment_.mark();
        if (assignment_.force(last, next) && !control_.stopped()) {
            Cost next_cost = cost + candidate.edge;
            if (plus(next_cost, assignment_.bound()) < incumbent_.cost()) {
                push(next);
                if (!assignment_.restrict_frontier(next, visited_) || control_.stopped()) {
                    pop(next); assignment_.rollback(checkpoint); return;
                }
                if (donate) scheduler_.publish(id_, Task{path_, next_cost, assignment_.state()});
                else visit(next_cost);
                pop(next);
            } else ++stats_.bound_prunes;
        }
        assignment_.rollback(checkpoint);
    }
    void visit(Cost cost) {
        if (control_.stopped()) return;
        ++stats_.nodes;
        Cost upper = incumbent_.cost();
        if (upper <= root_bound_) {
            if (upper < root_bound_) throw std::logic_error("incumbent below certified root bound");
            control_.prove(); scheduler_.wake(); return;
        }
        if (plus(cost, assignment_.bound()) >= upper) { ++stats_.bound_prunes; return; }
        const int last = path_.empty() ? graph_.n : path_.back();
        const int remaining = graph_.n - static_cast<int>(path_.size());
        if (remaining == 0) { incumbent_.offer(path_, cost, "complete search path"); return; }
        // This must run BEFORE history insertion. Otherwise an equal-cost prefix
        // discarded here could incorrectly dominate its canonical replacement later.
        if (window_dominated(graph_, path_)) { ++stats_.exchange_prunes; return; }
        const auto key = mix64(hash_ ^ mix64(static_cast<std::uint64_t>(last) + 0x4c415354ULL));
        const bool dominated = shared_ ? shared_->dominated(visited_, last, cost, key)
                                      : local_.dominated(visited_, last, cost, key);
        if (dominated) { ++stats_.history_prunes; return; }
        std::vector<int> completion;
        if (assignment_.completion(path_, completion)) {
            incumbent_.offer(completion, cost + assignment_.bound(), "assignment completion"); return;
        }
        if (remaining <= options_.tail_dp) {
            ++stats_.dp_calls;
            auto result = solve_tail(graph_, visited_, last, control_, stats_.nodes);
            if (result.complete && result.cost < INF) {
                auto path = path_; path.insert(path.end(), result.suffix.begin(), result.suffix.end());
                incumbent_.offer(path, cost + result.cost, "exact tail DP");
            }
            return;
        }
        // Apply the extra bound more often on smaller residual problems.
        if (options_.component_bound && (remaining <= 80 || path_.size() % 8 == 0)) {
            Cost extra = assignment_.component_extra();
            if (plus(plus(cost, assignment_.bound()), extra) >= incumbent_.cost()) { ++stats_.bound_prunes; return; }
        }
        if (control_.stopped()) return;
        // Infrequent feasible probes exploit duals encountered deeper in the search.
        if ((stats_.nodes & 8191U) == 0) {
            auto candidate = greedy(graph_, rng_, 1, control_, &assignment_.state(), path_);
            if (!candidate.empty()) {
                improve(graph_, candidate, control_, Clock::now() + std::chrono::milliseconds(2));
                incumbent_.offer(candidate, path_cost(graph_, candidate), "search probe");
            }
        }
        std::vector<Candidate> candidates;
        for (int v = 0; v < graph_.n; ++v) if (!bit(visited_, v) && degree_[v] == 0) {
            Cost edge = graph_.cost(last, v);
            Cost bound = plus(plus(cost, assignment_.bound()), assignment_.reduced(last, v));
            if (edge < INF && bound < incumbent_.cost()) candidates.push_back({v, bound, edge, false});
        }
        std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
            if (a.bound != b.bound) return a.bound < b.bound;
            if (a.edge != b.edge) return a.edge < b.edge;
            if (graph_.successors[a.vertex].size() != graph_.successors[b.vertex].size())
                return graph_.successors[a.vertex].size() > graph_.successors[b.vertex].size();
            return a.vertex < b.vertex;
        });
        // Keep the first branch local; donate other branches before entering it.
        for (std::size_t i = 1; i < candidates.size() && scheduler_.wants_work() && !control_.stopped(); ++i) {
            branch(candidates[i], cost, true); candidates[i].donated = true;
        }
        for (auto& c : candidates) {
            if (control_.stopped()) break;
            if (!c.donated) branch(c, cost, false);
        }
    }
};
} // namespace

Result solve(const Instance& input, const Options& options, Control& control) {
    Result result; result.n = input.n;
    Incumbent incumbent(input, control);
    Graph graph = Graph::build(input, control, [&](const std::vector<int>& order) {
        Cost cost = 0;
        for (std::size_t i = 1; i < order.size(); ++i) cost += input.at(order[i - 1], order[i]);
        incumbent.offer(order, cost, "topological order");
    });
    auto finish = [&] {
        result.path = incumbent.path();
        if (!result.path.empty()) {
            result.cost = input.validate(result.path);
            if (result.cost != incumbent.cost()) throw std::logic_error("final cost mismatch");
        }
        result.optimal = !result.path.empty() && control.reason() == Stop::exhausted;
        result.seconds = control.elapsed();
        return result;
    };
    if (control.stopped() || !graph.complete) return finish();
    if (graph.cyclic) {
        result.infeasible = true; control.prove(); log("precedence cycle: instance is infeasible"); return finish();
    }
    log("nodes = " + std::to_string(graph.n) + ", possible starts = " + std::to_string(graph.sources.size()) +
        ", possible ends after symmetry reduction = " + std::to_string(graph.sinks.size()) +
        ", twin-order edges = " + std::to_string(graph.symmetry_edges));
    if (graph.unique_order) { result.enumerated = 1; control.prove(); return finish(); }
    std::mt19937_64 rng(options.seed);
    auto initial = greedy(graph, rng, 0, control);
    if (!initial.empty()) incumbent.offer(initial, path_cost(graph, initial), "initial greedy");
    if (control.stopped()) return finish();
    Assignment root(graph, control);
    if (!root.initialize()) {
        if (!control.stopped()) throw std::logic_error("assignment infeasible despite an acyclic complete SOP input");
        return finish();
    }
    std::vector<int> matching_path;
    if (root.completion({}, matching_path)) incumbent.offer(matching_path, root.bound(), "root assignment");
    Cost root_bound = root.bound();
    if (options.component_bound && !control.stopped()) root_bound = plus(root_bound, root.component_extra());
    log("certified root lower bound = " + std::to_string(root_bound));
    if (control.stopped()) return finish();
    if (root_bound >= incumbent.cost()) {
        if (root_bound > incumbent.cost()) throw std::logic_error("root lower bound exceeds feasible incumbent");
        result.enumerated = 1; control.prove(); return finish();
    }
    std::size_t history_bytes = options.history_mb * std::size_t{1024} * 1024;
    // Avoid reserving hundreds of MiB for instances with only a few possible subsets.
    if (graph.n <= 18) {
        auto cap = (std::size_t{1} << graph.n) * static_cast<std::size_t>(graph.n) * 40;
        history_bytes = std::min(history_bytes, cap);
    }
    auto placement = placements(options.threads, options.affinity);
    std::ostringstream plan;
    plan << "workers = " << options.threads << ", history budget = " << history_bytes / (1024 * 1024)
         << " MiB (" << (options.shared_history ? "shared" : "local") << "), CPU/node:";
    for (const auto& p : placement) plan << ' ' << p.cpu << '/' << p.node;
    log(plan.str());
    Scheduler scheduler(options.threads, placement, control, Task{{}, 0, root.state()});
    SharedHistory history(options.threads, graph.words, history_bytes);
    std::vector<Statistics> stats(static_cast<std::size_t>(options.threads));
    std::vector<std::thread> threads;
    std::mutex error_mutex;
    std::exception_ptr error;
    double heuristic_seconds = options.heuristic_seconds < 0 ? std::min(0.5, std::max(0.02, graph.n * 0.002))
                                                           : options.heuristic_seconds;
    auto fail = [&] {
        { std::lock_guard<std::mutex> guard(error_mutex); if (!error) error = std::current_exception(); }
        control.fail(); scheduler.wake();
    };
    try {
        for (int id = 0; id < options.threads; ++id) threads.emplace_back([&, id] {
            try {
                if (!pin(placement[id])) log("warning: CPU binding failed for worker " + std::to_string(id));
                if (options.shared_history) history.initialize(id);
                Worker worker(id, graph, options, control, incumbent, scheduler,
                              options.shared_history ? &history : nullptr,
                              options.shared_history ? 0 : history_bytes / static_cast<std::size_t>(options.threads),
                              root_bound, stats[id]);
                if (scheduler.arrive()) {
                    // Allocate large history tables before starting the heuristic
                    // slice. Initialization still counts against the global deadline.
                    const auto warm_start = scheduler.start_time();
                    double available = control.deadline() == Clock::time_point::max()
                        ? std::numeric_limits<double>::infinity()
                        : std::max(0.0, std::chrono::duration<double>(control.deadline() - warm_start).count());
                    double slice = std::min(heuristic_seconds, available * 0.05);
                    auto warm_end = warm_start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(slice));
                    worker.run(root.state(), warm_end);
                }
            } catch (...) { fail(); }
        });
    } catch (...) { fail(); }
    for (auto& thread : threads) thread.join();
    if (error) std::rethrow_exception(error);
    for (int id = 0; id < options.threads; ++id) {
        const auto& s = stats[id]; result.enumerated += s.nodes;
        std::ostringstream line;
        line << "worker " << id << ": enumerated=" << s.nodes << ", tasks=" << s.tasks
             << ", bound-prunes=" << s.bound_prunes << ", dominance-prunes=" << s.history_prunes
             << ", interchange-prunes=" << s.exchange_prunes
             << ", tail-DP-calls=" << s.dp_calls;
        log(line.str());
    }
    if (control.reason() == Stop::exhausted) log("optimality proven: all improving alternatives eliminated");
    else if (control.reason() == Stop::timeout) log("time limit reached; optimality not proven");
    else if (control.reason() == Stop::interrupted) log("interrupted; optimality not proven");
    return finish();
}
} // namespace sop

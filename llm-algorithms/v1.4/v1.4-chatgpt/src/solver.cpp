#include "sop/solver.hpp"
#include "sop/assignment.hpp"
#include "sop/heuristic.hpp"
#include "sop/history.hpp"
#include "sop/topology.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace sop {
namespace {
struct Shared {
    const Instance& instance;
    const Deadline& deadline;
    History history;
    std::atomic<Cost> best{INF};
    std::atomic<bool> proven{false}, failed{false};
    Cost root_bound = -1;
    int connectivity = 1;
    std::mutex mutex;
    std::vector<int> best_path;
    std::exception_ptr error;
    double proof_seconds = -1;

    Shared(const Instance& p, const Options& o, const Deadline& d) : instance(p), deadline(d), history(o.history_mb, p.words), connectivity(o.connectivity) {}
    bool stopped() const { return proven.load(std::memory_order_relaxed) || failed.load(std::memory_order_relaxed) || deadline.expired(); }
    void prove() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failed.load() && !proven.load() && !deadline.expired()) {
            proof_seconds = deadline.elapsed();
            if (!deadline.expired()) proven.store(true, std::memory_order_release);
        }
    }
    void publish(const std::vector<int>& path) {
        if (path.empty() || deadline.expired()) return;
        Cost fast = 0;
        for (std::size_t i = 1; i < path.size(); ++i) fast += instance.raw(path[i - 1], path[i]);
        if (fast >= best.load(std::memory_order_relaxed)) return;
        // Only fully checked feasible paths can ever affect pruning.
        const Cost checked = validate_path(instance, path);
        std::lock_guard<std::mutex> lock(mutex);
        if (checked >= best.load(std::memory_order_relaxed) || deadline.expired()) return;
        best_path = path;
        best.store(checked, std::memory_order_release);
        std::cerr << "[incumbent] cost=" << checked << " time=" << std::fixed << std::setprecision(6) << deadline.elapsed() << "s\n";
        if (root_bound >= 0 && checked == root_bound && !deadline.expired()) {
            proof_seconds = deadline.elapsed();
            if (!deadline.expired()) proven.store(true, std::memory_order_release);
        }
    }
    void fail(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!error) error = e;
        failed.store(true, std::memory_order_release);
    }
    std::vector<int> incumbent() {
        std::lock_guard<std::mutex> lock(mutex);
        return best_path;
    }
};

struct Task {
    std::vector<int> path;
    Assignment bound;
    Cost prefix = 0;
};

class Scheduler {
public:
    Scheduler(unsigned count, const std::vector<Placement>& placements, Shared& shared)
        : count_(count), queues_(new Queue[count]), placement_(placements), shared_(shared) {}

    void put(unsigned worker, Task task) {
        // Increment first: the donor still owns a live task, so global
        // outstanding work cannot transiently become zero during donation.
        outstanding_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(queues_[worker].mutex);
            queues_[worker].tasks.push_back(std::move(task));
            queued_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_all();
    }
    bool wants_work() const {
        return count_ > 1 && idle_.load(std::memory_order_relaxed) != 0 && queued_.load(std::memory_order_relaxed) < count_ * 2;
    }
    bool get(unsigned worker, Task& task) {
        idle_.fetch_add(1, std::memory_order_relaxed);
        while (!shared_.stopped()) {
            if (take(worker, false, task)) { idle_.fetch_sub(1); return true; }
            // First steal within the NUMA node, then from other nodes.
            for (int pass = 0; pass < 2; ++pass) {
                for (unsigned offset = 1; offset < count_; ++offset) {
                    const unsigned victim = (worker + offset) % count_;
                    const bool same = placement_[worker].node == placement_[victim].node;
                    if (same != (pass == 0)) continue;
                    if (take(victim, true, task)) { idle_.fetch_sub(1); return true; }
                }
            }
            if (outstanding_.load(std::memory_order_acquire) == 0) break;
            std::unique_lock<std::mutex> lock(wait_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(2));
        }
        idle_.fetch_sub(1);
        return false;
    }
    void finish() {
        if (outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1) shared_.prove();
        cv_.notify_all();
    }
private:
    struct alignas(64) Queue { std::mutex mutex; std::deque<Task> tasks; };
    unsigned count_;
    std::unique_ptr<Queue[]> queues_;
    const std::vector<Placement>& placement_;
    Shared& shared_;
    std::atomic<std::size_t> outstanding_{0}, queued_{0};
    std::atomic<unsigned> idle_{0};
    std::condition_variable cv_;
    std::mutex wait_mutex_;
    bool take(unsigned victim, bool steal, Task& task) {
        std::lock_guard<std::mutex> lock(queues_[victim].mutex);
        auto& q = queues_[victim].tasks;
        if (q.empty()) return false;
        if (steal) { task = std::move(q.front()); q.pop_front(); }
        else { task = std::move(q.back()); q.pop_back(); }
        queued_.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }
};

class Worker {
public:
    Worker(unsigned id, const Instance& p, Shared& s, Scheduler& q)
        : id_(id), p_(p), shared_(s), scheduler_(q), scratch_(p.n + 1), frames_(p.n + 1), degree_(p.indegree) {}
    std::uint64_t nodes = 0;
    void run() {
        Task task;
        while (scheduler_.get(id_, task)) {
            path_ = std::move(task.path);
            visited_ = Bits{};
            degree_ = p_.indegree;
            for (int v : path_) {
                insert(visited_, v);
                for (int s : p_.successors[v]) --degree_[s];
            }
            auto& root = frame(path_.size());
            root = std::move(task.bound);
            dfs(root, task.prefix);
            scheduler_.finish();
        }
    }
private:
    struct Candidate { int vertex; Cost lower; bool donated = false; };
    unsigned id_;
    const Instance& p_;
    Shared& shared_;
    Scheduler& scheduler_;
    AssignmentScratch scratch_;
    std::vector<std::unique_ptr<Assignment>> frames_;
    std::vector<int> path_, degree_;
    Bits visited_{};

    Assignment& frame(std::size_t depth) {
        if (!frames_[depth]) frames_[depth] = std::make_unique<Assignment>();
        return *frames_[depth];
    }
    void push(int v) {
        path_.push_back(v); insert(visited_, v);
        for (int s : p_.successors[v]) --degree_[s];
    }
    void pop(int v) {
        for (int s : p_.successors[v]) ++degree_[s];
        erase(visited_, v); path_.pop_back();
    }
    // A matching can already describe one complete path. It must also pass
    // precedence checks before it can be used as a feasible upper bound.
    void try_completion(const Assignment& a) {
        auto suffix = a.completion(p_);
        if (suffix.size() + path_.size() != static_cast<std::size_t>(p_.n)) return;
        Bits placed = visited_;
        for (int v : suffix) {
            if (contains(placed, v) || !subset(p_.pred[v], placed, p_.words)) return;
            insert(placed, v);
        }
        auto candidate = path_;
        candidate.insert(candidate.end(), suffix.begin(), suffix.end());
        shared_.publish(candidate);
    }
    void child(const Assignment& a, const Candidate& candidate, Cost prefix, bool donate) {
        if (shared_.stopped() || candidate.lower >= shared_.best.load(std::memory_order_relaxed)) return;
        const int v = candidate.vertex;
        const Cost next_prefix = prefix + p_.edge(a.last, v);
        push(v);
        auto& next = frame(path_.size());
        next = a;
        ++nodes;
        const auto status = next.extend(p_, v, visited_, scratch_, shared_.deadline);
        if (status == BoundStatus::feasible && !shared_.stopped()) {
            if (donate) scheduler_.put(id_, Task{path_, next, next_prefix});
            else dfs(next, next_prefix);
        }
        pop(v);
    }
    void dfs(const Assignment& a, Cost prefix) {
        if (shared_.stopped()) return;
        const Cost lower = prefix + a.value;
        if (lower >= shared_.best.load(std::memory_order_relaxed)) return;
        if (shared_.history.dominated(visited_, a.last, prefix)) return;
        if (shared_.connectivity == 2 || (shared_.connectivity == 1 && path_.size() % 8 == 0 && a.rows.size() >= 8)) {
            const Cost penalty = a.connectivity_penalty(p_, visited_, shared_.deadline);
            if (penalty == INF || lower + penalty >= shared_.best.load(std::memory_order_relaxed)) return;
        }
        try_completion(a);
        if (shared_.stopped() || lower >= shared_.best.load(std::memory_order_relaxed)) return;
        std::vector<Candidate> candidates;
        for (int v = 0; v < p_.n; ++v) if (!degree_[v] && !contains(visited_, v) && p_.edge(a.last, v) != INF) {
            const Cost bound = lower + a.reduced_cost(p_, v);
            if (bound < shared_.best.load(std::memory_order_relaxed)) candidates.push_back({v, bound, false});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) {
            return x.lower < y.lower || (x.lower == y.lower && x.vertex < y.vertex);
        });
        // Donate alternative branches while retaining the most promising one.
        // Queue size stays bounded; each task holds O(N) state, not a full tree.
        for (std::size_t k = 1; k < candidates.size() && scheduler_.wants_work(); ++k) {
            child(a, candidates[k], prefix, true);
            candidates[k].donated = true;
            if (shared_.stopped()) return;
        }
        for (const auto& c : candidates) if (!c.donated) {
            child(a, c, prefix, false);
            if (shared_.stopped()) return;
        }
    }
};
} // namespace

Result solve(const Instance& p, const Options& options, const Deadline& deadline) {
    Result result;
    if (deadline.expired()) return result;
    if (!p.acyclic) { result.infeasible = true; return result; }
    Shared shared(p, options, deadline);
    std::mt19937_64 rng(options.seed);
    auto initial = greedy_path(p, rng, 0, [&] { return deadline.expired(); });
    shared.publish(initial);
    Assignment root;
    AssignmentScratch scratch(p.n + 1);
    ++result.nodes;
    const auto root_status = root.initialize(p, Bits{}, scratch, deadline);
    if (root_status == BoundStatus::infeasible && !deadline.expired()) {
        throw std::logic_error("Acyclic complete SOP unexpectedly has an infeasible assignment");
    }
    if (root_status == BoundStatus::feasible) {
        shared.root_bound = root.value;
        if (options.connectivity) {
            const Cost penalty = root.connectivity_penalty(p, Bits{}, deadline);
            if (penalty == INF) throw std::logic_error("Acyclic SOP has a disconnected relaxation");
            shared.root_bound += penalty;
        }
        std::cerr << "[bound] root_assignment=" << root.value << " root_lower_bound=" << shared.root_bound << '\n';
        if (shared.best.load() == shared.root_bound) shared.prove();
    }
    if (root_status == BoundStatus::feasible && !shared.stopped()) {
        const bool dedicated_heuristic = options.threads >= 4 && options.heuristic_seconds > 0 && p.n > 16;
        const unsigned search_threads = options.threads - (dedicated_heuristic ? 1U : 0U);
        const auto placement = discover_placement(options.threads);
        std::cerr << "[parallel] search_workers=" << search_threads << " heuristic_workers=" << (dedicated_heuristic ? 1 : 0) << " pin=" << (options.pin ? "auto" : "off") << '\n';
        Scheduler scheduler(search_threads, placement, shared);
        scheduler.put(0, Task{{}, std::move(root), 0});
        std::vector<std::thread> threads;
        std::vector<std::uint64_t> counts(options.threads, 0);
        try {
            for (unsigned id = 0; id < options.threads; ++id) {
                threads.emplace_back([&, id] {
                    try {
                        if (shared.stopped()) return;
                        if (options.pin && placement[id].cpu >= 0 && !bind_worker(placement[id].cpu)) {
                            std::lock_guard<std::mutex> lock(shared.mutex);
                            std::cerr << "[placement] unable to pin worker " << id << "; continuing unpinned\n";
                        }
                        // Private read-only matrices and mutable state are allocated
                        // by their bound worker: first-touch locality on NUMA systems.
                        const Instance local = p;
                        const auto phase_start = Clock::now();
                        const double budget = p.n <= 16 ? 0.0 : std::min(options.heuristic_seconds, std::max(0.0, deadline.remaining() * 0.1));
                        const bool heuristic_only = id >= search_threads;
                        if (budget > 0 || heuristic_only) {
                            const auto stop = [&] {
                                return shared.stopped() || (!heuristic_only && std::chrono::duration<double>(Clock::now() - phase_start).count() >= budget);
                            };
                            heuristic_search(local, options.seed + 0x9e3779b97f4a7c15ULL * (id + 1), shared.incumbent(), stop,
                                             [&](const std::vector<int>& path) { shared.publish(path); }, [&] { return shared.incumbent(); });
                        }
                        if (!heuristic_only) {
                            Worker worker(id, local, shared, scheduler);
                            worker.run();
                            counts[id] = worker.nodes;
                        }
                    } catch (...) { shared.fail(std::current_exception()); }
                });
            }
        } catch (...) { shared.fail(std::current_exception()); }
        for (auto& thread : threads) if (thread.joinable()) thread.join();
        for (auto count : counts) result.nodes += count;
        if (shared.error) std::rethrow_exception(shared.error);
    }
    result.path = shared.incumbent();
    if (!result.path.empty()) result.cost = validate_path(p, result.path);
    result.optimal = shared.proven.load() && result.cost >= 0;
    result.proof_seconds = shared.proof_seconds;
    std::cerr << "[search] nodes=" << result.nodes << " history_hits=" << shared.history.hits() << '\n';
    if (result.optimal) std::cerr << "[proof] optimal cost=" << result.cost << " completed_at=" << result.proof_seconds << "s\n";
    else if (deadline.expired()) std::cerr << (interrupt_requested.load() ? "[interrupted]" : "[timeout]") << " search stopped without an optimality proof\n";
    return result;
}
} // namespace sop

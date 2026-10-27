#include "sop/cli.hpp"
#include "sop/instance.hpp"
#include "sop/solver.hpp"
#include <exception>
#include <csignal>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") { sop::print_help(); return 0; }
    if (argc == 2 && std::string(argv[1]) == "--version") { std::cout << "sop_exact 1.0.0 (C++17)\n"; return 0; }
    try {
        const auto options = sop::parse_options(argc, argv);
        std::signal(SIGINT, [](int) { sop::interrupt_requested.store(true, std::memory_order_relaxed); });
        std::signal(SIGTERM, [](int) { sop::interrupt_requested.store(true, std::memory_order_relaxed); });
        const auto start = sop::Clock::now();
        const sop::Deadline deadline(start, options.time_limit);
        auto instance = sop::read_instance(options.input);
        sop::Result result;
        if (sop::preprocess(instance, deadline)) {
            result = sop::solve(instance, options, deadline);
        } else std::cerr << "[timeout] limit reached during input or preprocessing\n";
        if (!result.path.empty()) {
            const auto checked = sop::validate_path(instance, result.path);
            if (checked != result.cost) throw std::runtime_error("Final cost does not match independently recomputed cost");
        }
        if (result.infeasible) std::cerr << "[infeasible] precedence constraints contain a directed cycle\n";
        const double seconds = deadline.elapsed();
        sop::write_csv(options, sop::csv_row(options, instance.n, seconds, result));
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}

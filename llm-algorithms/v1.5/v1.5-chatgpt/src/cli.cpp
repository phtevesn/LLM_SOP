// Command-line validation and RFC-style CSV escaping; POSIX append locking keeps
// a shared output file's header and rows intact across concurrent solver processes.
#include "sop/cli.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#ifdef __unix__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sop {
namespace {
std::uint64_t number(const std::string& text, std::uint64_t maximum, const std::string& option) {
    std::uint64_t value = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > maximum)
        throw std::invalid_argument("invalid value for " + option + ": " + text);
    return value;
}
double seconds(const std::string& text, const std::string& option) {
    std::size_t end = 0;
    double value;
    try { value = std::stod(text, &end); } catch (...) { throw std::invalid_argument("invalid value for " + option); }
    if (end != text.size() || !std::isfinite(value) || value < 0 || value > 1.0e9)
        throw std::invalid_argument(option + " must be finite and in 0..1000000000");
    return value;
}
std::string csv(const std::string& text) {
    if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
    std::string escaped = "\"";
    for (char c : text) { if (c == '"') escaped += '"'; escaped += c; }
    return escaped + '"';
}
void append_file(const std::string& path, const std::string& header, const std::string& row, bool include_header) {
#ifdef __unix__
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd < 0) throw std::runtime_error("cannot open output: " + std::string(std::strerror(errno)));
    try {
        while (::flock(fd, LOCK_EX) != 0) if (errno != EINTR) throw std::runtime_error("cannot lock output file");
        struct stat status{};
        if (::fstat(fd, &status) != 0) throw std::runtime_error("cannot inspect output file");
        std::string data = (include_header && status.st_size == 0 ? header : "") + row;
        const char* ptr = data.data(); std::size_t left = data.size();
        while (left) {
            auto written = ::write(fd, ptr, left);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("cannot write output: " + std::string(std::strerror(errno)));
            ptr += written; left -= static_cast<std::size_t>(written);
        }
    } catch (...) { ::close(fd); throw; }
    if (::close(fd) != 0) throw std::runtime_error("cannot close output file");
#else
    std::error_code ec;
    bool empty = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
    std::ofstream out(path, std::ios::app);
    if (!out) throw std::runtime_error("cannot open output file");
    if (empty && include_header) out << header;
    out << row; out.flush();
    if (!out) throw std::runtime_error("cannot write output file");
#endif
}
} // namespace

std::string usage() {
    return "Usage: sop_exact FILE.sop [--time-limit SECONDS] [--threads N]\n"
           "                 [--output FILE.csv] [--no-header]\n"
           "Tuning: --history-mb MB       Total history budget, default 256; 0 disables\n"
           "        --history-mode MODE  shared (default) or local\n"
           "        --tail-dp N          Exact suffix threshold 0..20, default 12\n"
           "        --heuristic-seconds S  Initial parallel heuristic budget, auto by default\n"
           "        --affinity MODE      auto (default) or none\n"
           "        --component-bound MODE  on (default) or off\n"
           "        --seed N             Random seed, default 1\n"
           "Defaults: no time limit, one worker, stdout with header.\n"
           "A time limit of 0 requests immediate timeout. Use -- before a filename starting with '-'.\n";
}
Options parse_options(int argc, char** argv) {
    Options options;
    bool positional_only = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!positional_only && arg == "--") { positional_only = true; continue; }
        auto value = [&]() -> std::string {
            if (++i >= argc) throw std::invalid_argument("missing value for " + arg);
            return argv[i];
        };
        if (!positional_only && arg == "--time-limit") options.time_limit = seconds(value(), arg);
        else if (!positional_only && arg == "--threads") {
            options.threads = static_cast<int>(number(value(), 1024, arg));
            if (!options.threads) throw std::invalid_argument("--threads must be positive");
        } else if (!positional_only && arg == "--output") options.output = value();
        else if (!positional_only && arg == "--no-header") options.header = false;
        else if (!positional_only && arg == "--history-mb") options.history_mb = number(value(), 1048576, arg);
        else if (!positional_only && arg == "--tail-dp") options.tail_dp = static_cast<int>(number(value(), 20, arg));
        else if (!positional_only && arg == "--seed") options.seed = number(value(), UINT64_MAX, arg);
        else if (!positional_only && arg == "--heuristic-seconds") options.heuristic_seconds = seconds(value(), arg);
        else if (!positional_only && arg == "--history-mode") {
            auto v = value();
            if (v != "local" && v != "shared") throw std::invalid_argument("--history-mode must be shared or local");
            options.shared_history = v == "shared";
        } else if (!positional_only && arg == "--affinity") {
            auto v = value();
            if (v != "auto" && v != "none") throw std::invalid_argument("--affinity must be auto or none");
            options.affinity = v == "auto";
        } else if (!positional_only && arg == "--component-bound") {
            auto v = value();
            if (v != "on" && v != "off") throw std::invalid_argument("--component-bound must be on or off");
            options.component_bound = v == "on";
        } else if (!positional_only && !arg.empty() && arg[0] == '-') throw std::invalid_argument("unknown option: " + arg);
        else {
            if (!options.input.empty()) throw std::invalid_argument("provide exactly one input file");
            options.input = arg;
        }
    }
    if (options.input.empty()) throw std::invalid_argument("missing input file");
    if (!options.output.empty()) {
        std::error_code ec;
        if (std::filesystem::equivalent(options.input, options.output, ec) ||
            std::filesystem::absolute(options.input).lexically_normal() == std::filesystem::absolute(options.output).lexically_normal())
            throw std::invalid_argument("output must not overwrite or append to the input instance");
    }
    return options;
}
void write_result(const Options& options, const Result& result) {
    const std::string header = "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal\n";
    std::ostringstream path;
    for (std::size_t i = 0; i < result.path.size(); ++i) { if (i) path << ' '; path << result.path[i]; }
    std::ostringstream row;
    row.imbue(std::locale::classic());
    row << csv(std::filesystem::path(options.input).filename().string()) << ',' << result.n << ','
        << std::fixed << std::setprecision(6) << result.seconds << ',' << result.enumerated << ','
        << result.cost << ',' << csv(path.str()) << ',' << (result.optimal ? "yes" : "no") << '\n';
    // Stdout remains useful even if the additional file cannot be written.
    if (options.header) std::cout << header;
    std::cout << row.str(); std::cout.flush();
    if (!std::cout) throw std::runtime_error("cannot write stdout");
    if (!options.output.empty()) append_file(options.output, header, row.str(), options.header);
}
} // namespace sop

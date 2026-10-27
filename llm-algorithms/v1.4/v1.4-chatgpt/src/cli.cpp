#include "sop/cli.hpp"
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef __unix__
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sop {
namespace {
constexpr const char* HEADER = "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal\n";
std::uint64_t unsigned_integer(const std::string& s, const std::string& flag) {
    std::uint64_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) throw std::runtime_error("Invalid integer for " + flag);
    return v;
}
double nonnegative_number(const std::string& s, const std::string& flag) {
    std::size_t end = 0;
    const double value = std::stod(s, &end);
    if (end != s.size() || !std::isfinite(value) || value < 0) throw std::runtime_error("Invalid nonnegative seconds for " + flag);
    return value;
}
std::string quote(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string result = "\"";
    for (char c : s) { result += c; if (c == '"') result += '"'; }
    return result + '"';
}
} // namespace

void print_help() {
    std::cout << "Usage: sop_solver FILE.sop [options]\n"
              << "  --time-limit SECONDS      Wall-clock limit, including input/preprocessing (default unlimited)\n"
              << "  --threads N               Total worker threads, 1..4096 (default 1)\n"
              << "  --output FILE.csv         Append CSV to this file and also write stdout\n"
              << "  --no-header               Suppress both CSV headers\n"
              << "  --history-mb N            Total history-cache budget in MiB; 0 disables (default 256)\n"
              << "  --heuristic-seconds S     Initial heuristic budget per worker (default 0.5)\n"
              << "                            With >=4 threads, one worker continues heuristics; 0 disables\n"
              << "  --seed N                  Heuristic random seed (default 1)\n"
              << "  --pin auto|off            Discover topology and pin workers (default auto)\n"
              << "  --connectivity auto|off|always  Reduced-cost component bound (default auto)\n"
              << "  --help                    Show this help\n"
              << "  --version                 Show version\n";
}

Options parse_options(int argc, char** argv) {
    Options o;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-header") { o.header = false; continue; }
        if (arg.rfind("--", 0) == 0) {
            if (!seen.insert(arg).second) throw std::runtime_error("Duplicate option: " + arg);
            if (arg != "--time-limit" && arg != "--threads" && arg != "--output" && arg != "--history-mb" &&
                arg != "--heuristic-seconds" && arg != "--seed" && arg != "--pin" && arg != "--connectivity") throw std::runtime_error("Unknown option: " + arg);
            if (++i == argc) throw std::runtime_error("Missing value for " + arg);
            const std::string value = argv[i];
            if (arg == "--time-limit") o.time_limit = nonnegative_number(value, arg);
            else if (arg == "--heuristic-seconds") o.heuristic_seconds = nonnegative_number(value, arg);
            else if (arg == "--threads") {
                const auto count = unsigned_integer(value, arg);
                if (count < 1 || count > 4096) throw std::runtime_error("--threads must be 1..4096");
                o.threads = static_cast<unsigned>(count);
            } else if (arg == "--history-mb") {
                const auto count = unsigned_integer(value, arg);
                if (count > 1048576) throw std::runtime_error("--history-mb must be 0..1048576");
                o.history_mb = static_cast<std::size_t>(count);
            } else if (arg == "--seed") o.seed = unsigned_integer(value, arg);
            else if (arg == "--output") o.output = value;
            else if (arg == "--connectivity") {
                if (value != "auto" && value != "off" && value != "always") throw std::runtime_error("--connectivity must be auto, off or always");
                o.connectivity = value == "off" ? 0 : value == "always" ? 2 : 1;
            }
            else if (arg == "--pin") {
                if (value != "auto" && value != "off") throw std::runtime_error("--pin must be auto or off");
                o.pin = value == "auto";
            }
        } else {
            if (!o.input.empty()) throw std::runtime_error("Exactly one input file is required");
            o.input = arg;
        }
    }
    if (o.input.empty()) throw std::runtime_error("Missing input file; see --help");
    return o;
}

std::string csv_row(const Options& o, int n, double seconds, const Result& r) {
    const auto slash = o.input.find_last_of("/\\");
    const auto name = slash == std::string::npos ? o.input : o.input.substr(slash + 1);
    std::ostringstream out;
    out << quote(name) << ',' << n << ',' << std::fixed << std::setprecision(6) << seconds << ','
        << r.nodes << ',' << r.cost << ',';
    for (std::size_t i = 0; i < r.path.size(); ++i) { if (i) out << ' '; out << r.path[i]; }
    out << ',' << (r.optimal ? "yes" : "no") << '\n';
    return out.str();
}

void write_csv(const Options& o, const std::string& row) {
    if (!o.output.empty()) {
#ifdef __unix__
        // Serialize appends from independent solver processes as well as check
        // the header under the same lock. Never truncate an existing result file.
        const int fd = open(o.output.c_str(), O_RDWR | O_CREAT | O_APPEND, 0666);
        if (fd < 0) throw std::runtime_error("Cannot open output: " + o.output + ": " + std::strerror(errno));
        struct Close { int fd; ~Close() { close(fd); } } close_file{fd};
        if (flock(fd, LOCK_EX) != 0) throw std::runtime_error("Cannot lock output file");
        struct stat info{};
        if (fstat(fd, &info) != 0) throw std::runtime_error("Cannot inspect output file");
        struct stat input_info{};
        if (stat(o.input.c_str(), &input_info) == 0 && input_info.st_dev == info.st_dev && input_info.st_ino == info.st_ino)
            throw std::runtime_error("Output file must differ from the input file");
        std::string separator;
        if (info.st_size > 0) {
            char last = '\n';
            if (pread(fd, &last, 1, info.st_size - 1) != 1) throw std::runtime_error("Cannot inspect last byte of output");
            if (last != '\n') separator = "\n";
        }
        const std::string text = separator + (o.header && info.st_size == 0 ? std::string(HEADER) : "") + row;
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto bytes = write(fd, text.data() + offset, text.size() - offset);
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes <= 0) throw std::runtime_error("Cannot write output file");
            offset += static_cast<std::size_t>(bytes);
        }
#else
        std::ifstream existing(o.output, std::ios::binary | std::ios::ate);
        const bool empty = !existing || existing.tellg() == 0;
        std::ofstream out(o.output, std::ios::app);
        if (!out) throw std::runtime_error("Cannot open output: " + o.output);
        if (o.header && empty) out << HEADER;
        out << row; out.flush();
        if (!out) throw std::runtime_error("Cannot write output file");
#endif
    }
    if (o.header) std::cout << HEADER;
    std::cout << row << std::flush;
    if (!std::cout) throw std::runtime_error("Cannot write stdout");
}
} // namespace sop

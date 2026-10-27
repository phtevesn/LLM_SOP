#include "cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common.hpp"

namespace sop {

void printUsage() {
    std::fprintf(stderr,
                 "Usage: sop_solver <file.sop> [--time-limit SECONDS] [--threads N]\n"
                 "                  [--output out.csv] [--no-header]\n"
                 "                  [--seed S] [--memory-gb G] [--no-pin]\n"
                 "\n"
                 "  --time-limit SECONDS  wall-clock limit for the whole run (default: none)\n"
                 "  --threads N           worker threads (default: 1)\n"
                 "  --output out.csv      also append the result row to this CSV file\n"
                 "  --no-header           never write the CSV header row\n"
                 "  --seed S              random seed of the heuristic (default: 12345)\n"
                 "  --memory-gb G         size of the dominance table in GiB (default: auto)\n"
                 "  --no-pin              do not pin threads / replicate data per NUMA node\n"
                 "  --no-heuristic        debugging aid: disable the ILS (branch-and-bound only)\n");
}

CliOptions parseCli(int argc, char** argv) {
    CliOptions o;
    auto need = [&](int& i, const char* name) -> const char* {
        if (i + 1 >= argc) throw FatalError(std::string("missing value for ") + name);
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) o.help = true;
        else if (!std::strcmp(a, "--time-limit")) {
            const char* v = need(i, a);
            char* e = nullptr;
            o.timeLimit = std::strtod(v, &e);
            if (e == v || *e || o.timeLimit < 0) throw FatalError("invalid --time-limit value");
        } else if (!std::strcmp(a, "--threads")) {
            const char* v = need(i, a);
            char* e = nullptr;
            long t = std::strtol(v, &e, 10);
            if (e == v || *e || t < 1 || t > 4096) throw FatalError("invalid --threads value");
            o.threads = (int)t;
        } else if (!std::strcmp(a, "--output")) o.output = need(i, a);
        else if (!std::strcmp(a, "--no-header")) o.noHeader = true;
        else if (!std::strcmp(a, "--no-pin")) o.noPin = true;
        else if (!std::strcmp(a, "--no-heuristic")) o.noHeuristic = true;
        else if (!std::strcmp(a, "--seed")) o.seed = std::strtoull(need(i, a), nullptr, 10);
        else if (!std::strcmp(a, "--memory-gb")) {
            const char* v = need(i, a);
            char* e = nullptr;
            o.memoryGb = std::strtod(v, &e);
            if (e == v || *e || o.memoryGb < 0) throw FatalError("invalid --memory-gb value");
        } else if (a[0] == '-' && a[1] == '-') throw FatalError(std::string("unknown option ") + a);
        else if (o.input.empty()) o.input = a;
        else throw FatalError("more than one input file given");
    }
    if (!o.help && o.input.empty()) throw FatalError("no input file given");
    return o;
}

}  // namespace sop

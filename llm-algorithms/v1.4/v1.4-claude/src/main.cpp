// sop_solver - exact parallel solver for the Sequential Ordering Problem.
#include <algorithm>
#include <cstdio>
#include <string>

#include "cli.hpp"
#include "common.hpp"
#include "instance.hpp"
#include "output.hpp"
#include "solver.hpp"
#include "validate.hpp"

using namespace sop;

namespace {
std::string baseName(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        CliOptions cli = parseCli(argc, argv);
        if (cli.help) { printUsage(); return 0; }

        RawInstance raw = parseSopFile(cli.input);
        logMsg("instance %s: N=%d", baseName(cli.input).c_str(), raw.n);
        Problem P = buildProblem(raw);
        if (P.feasible)
            logMsg("preprocessing: start=%d end=%d dummies=%d", P.start, P.end,
                   (int)P.addedSource + (int)P.addedSink);

        SolverOptions so;
        so.timeLimit = cli.timeLimit;
        so.threads = cli.threads;
        so.seed = cli.seed;
        so.memoBytes = (size_t)(cli.memoryGb * 1073741824.0);
        so.pin = !cli.noPin;
        so.noHeuristic = cli.noHeuristic;
        SolveResult res = solve(P, so);

        ResultRow row;
        row.instance = baseName(cli.input);
        row.instanceSize = raw.n;
        row.enumeratedNodes = res.nodes;
        row.bestCost = -1;
        row.optimal = false;
        if (res.feasibleFound) {
            Validation v = validatePath(raw, res.path);
            if (!v.ok) {
                logMsg("INTERNAL ERROR: final path failed validation: %s", v.error.c_str());
                row.optimal = false;
            } else {
                if (v.cost != res.cost)
                    logMsg("INTERNAL ERROR: cost mismatch (solver %lld, recomputed %lld)",
                           (long long)res.cost, (long long)v.cost);
                row.bestCost = v.cost;  // trust the from-scratch recomputation
                row.bestPath = res.path;
                row.optimal = res.optimal && v.cost == res.cost;
                logMsg("validation OK: permutation, precedence, cost %lld", (long long)v.cost);
            }
        } else {
            logMsg("no feasible solution found");
        }
        row.timeSeconds = nowSeconds();
        logMsg("result: cost %lld, %s, %llu nodes", (long long)row.bestCost,
               row.optimal ? "proven optimal" : "NOT proven optimal", (unsigned long long)row.enumeratedNodes);
        return emitResult(row, cli.output, cli.noHeader) ? 0 : 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

// cli.hpp - command-line parsing.
#pragma once

#include <string>

namespace sop {

struct CliOptions {
    std::string input;
    double timeLimit = -1;       // seconds, < 0: none
    int threads = 1;
    std::string output;          // CSV path ("" = stdout only)
    bool noHeader = false;
    unsigned long long seed = 12345;
    double memoryGb = 0;         // 0: automatic
    bool noPin = false;
    bool noHeuristic = false;   // debugging: branch-and-bound only (no ILS)
    bool help = false;
};

// Throws FatalError on malformed arguments.
CliOptions parseCli(int argc, char** argv);
void printUsage();

}  // namespace sop

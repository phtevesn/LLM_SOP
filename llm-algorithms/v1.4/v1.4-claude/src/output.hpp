// output.hpp - CSV result row formatting and writing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sop {

struct ResultRow {
    std::string instance;
    int instanceSize = 0;
    double timeSeconds = 0;
    uint64_t enumeratedNodes = 0;
    int64_t bestCost = -1;
    std::vector<int> bestPath;
    bool optimal = false;
};

std::string csvHeader();
std::string csvRow(const ResultRow& r);

// Prints to stdout (header unless noHeader). If outputPath is non-empty the row is also
// appended to that file; the header is written there only if the file is new/empty and
// noHeader is false. Returns false (after logging) if the file could not be written.
bool emitResult(const ResultRow& r, const std::string& outputPath, bool noHeader);

}  // namespace sop

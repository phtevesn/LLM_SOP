#include "output.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "common.hpp"

namespace sop {

namespace {
std::string csvQuote(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += '"';
        o += c;
    }
    return o + "\"";
}
}  // namespace

std::string csvHeader() { return "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal"; }

std::string csvRow(const ResultRow& r) {
    std::ostringstream os;
    char tbuf[64];
    std::snprintf(tbuf, sizeof(tbuf), "%.3f", r.timeSeconds);
    std::ostringstream path;
    for (size_t i = 0; i < r.bestPath.size(); ++i) path << (i ? " " : "") << r.bestPath[i];
    os << csvQuote(r.instance) << ',' << r.instanceSize << ',' << tbuf << ',' << r.enumeratedNodes << ','
       << r.bestCost << ',' << path.str() << ',' << (r.optimal ? "yes" : "no");
    return os.str();
}

bool emitResult(const ResultRow& r, const std::string& outputPath, bool noHeader) {
    const std::string row = csvRow(r);
    if (!noHeader) std::printf("%s\n", csvHeader().c_str());
    std::printf("%s\n", row.c_str());
    std::fflush(stdout);
    if (outputPath.empty()) return true;
    struct stat st;
    bool needHeader = !noHeader && (stat(outputPath.c_str(), &st) != 0 || st.st_size == 0);
    std::ofstream f(outputPath, std::ios::app);
    if (!f) {
        logMsg("error: cannot open output file %s", outputPath.c_str());
        return false;
    }
    if (needHeader) f << csvHeader() << '\n';
    f << row << '\n';
    f.flush();
    if (!f) {
        logMsg("error: failed writing to %s", outputPath.c_str());
        return false;
    }
    return true;
}

}  // namespace sop

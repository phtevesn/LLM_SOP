#include "validate.hpp"

namespace sop {

Validation validatePath(const RawInstance& raw, const std::vector<int>& path) {
    Validation v;
    const int n = raw.n;
    if ((int)path.size() != n) {
        v.error = "path length " + std::to_string(path.size()) + " != " + std::to_string(n);
        return v;
    }
    std::vector<int> pos(n, -1);
    for (int k = 0; k < n; ++k) {
        int x = path[k];
        if (x < 0 || x >= n) { v.error = "node index out of range"; return v; }
        if (pos[x] != -1) { v.error = "node visited twice: " + std::to_string(x); return v; }
        pos[x] = k;
    }
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j && raw.at(i, j) < 0 && pos[j] > pos[i]) {
                v.error = "precedence violated: " + std::to_string(i) + " depends on " + std::to_string(j);
                return v;
            }
    int64_t c = 0;
    for (int k = 0; k + 1 < n; ++k) {
        int64_t t = raw.at(path[k], path[k + 1]);
        if (t < 0) { v.error = "negative arc used"; return v; }
        c += t;
    }
    v.ok = true;
    v.cost = c;
    return v;
}

}  // namespace sop

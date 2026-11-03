#include "incumbent.hpp"

#include "common.hpp"

namespace sop {

bool Incumbent::offer(int64_t cost, const std::vector<int>& path, const char* source) {
    if (cost >= best_.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lk(m_);
    if (cost >= best_.load(std::memory_order_relaxed)) return false;
    path_ = path;
    best_.store(cost, std::memory_order_relaxed);
    logMsg("new best cost: %lld (%s)", (long long)cost, source);
    return true;
}

std::vector<int> Incumbent::path() const {
    std::lock_guard<std::mutex> lk(m_);
    return path_;
}

}  // namespace sop

#include "common.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace sop {

namespace {
using Clock = std::chrono::steady_clock;
const Clock::time_point g_processStart = Clock::now();
std::mutex g_logMutex;
}  // namespace

double nowSeconds() {
    return std::chrono::duration<double>(Clock::now() - g_processStart).count();
}

void logMsg(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::fprintf(stderr, "[%9.3fs] %s\n", nowSeconds(), buf);
    std::fflush(stderr);
}

}  // namespace sop

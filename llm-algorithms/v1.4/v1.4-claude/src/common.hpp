// common.hpp - shared constants, timing and logging helpers.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace sop {

// Marker stored in the cost matrix for arcs that can never appear in a
// feasible path (precedence-violating or implied-by-transitivity arcs).
constexpr int32_t kForbidden = INT32_MAX;
// "Infinity" for 64-bit dual/cost arithmetic (leaves head-room for additions).
constexpr int64_t kInf = INT64_MAX / 4;

// Seconds elapsed since process start (monotonic wall clock).
double nowSeconds();

// printf-style logging to stderr, prefixed with the elapsed time. Thread-safe.
void logMsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Error type for unrecoverable input / usage problems.
struct FatalError : public std::runtime_error {
    explicit FatalError(const std::string& m) : std::runtime_error(m) {}
};

}  // namespace sop

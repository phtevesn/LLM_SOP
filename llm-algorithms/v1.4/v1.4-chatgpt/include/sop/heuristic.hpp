#pragma once
#include "sop/instance.hpp"
#include <functional>
#include <random>

namespace sop {
using Stop = std::function<bool()>;
using Publish = std::function<void(const std::vector<int>&)>;
using Incumbent = std::function<std::vector<int>()>;

std::vector<int> greedy_path(const Instance&, std::mt19937_64&, int variation, const Stop&);
void improve_path(const Instance&, std::vector<int>&, const Stop&);
void heuristic_search(const Instance&, std::uint64_t seed, const std::vector<int>& initial,
                      const Stop&, const Publish&, const Incumbent&);
} // namespace sop

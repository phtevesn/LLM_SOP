#pragma once
#include "sop/common.hpp"

namespace sop {
Options parse_options(int argc, char** argv);
std::string usage();
void write_result(const Options&, const Result&);
} // namespace sop

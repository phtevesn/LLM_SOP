#pragma once
#include "sop/types.hpp"
#include <string>

namespace sop {
Options parse_options(int argc, char** argv);
void print_help();
std::string csv_row(const Options&, int n, double seconds, const Result&);
void write_csv(const Options&, const std::string& row);
} // namespace sop

#ifndef SOP_VALIDATOR_HPP
#define SOP_VALIDATOR_HPP

#include "sop_instance.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sop {

/**
 * @brief Independent from-scratch path validator verifying permutation completeness,
 *        all raw matrix precedence constraints, and exact objective cost sum.
 */
struct Validator {
    static bool validate(const Instance& inst, const std::vector<int>& path, int64_t reported_cost, std::string& err_msg);
};

} // namespace sop

#endif // SOP_VALIDATOR_HPP
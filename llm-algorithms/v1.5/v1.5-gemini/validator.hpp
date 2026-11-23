/*
 * File: validator.hpp
 * Purpose: Declares postprocessing routines: independent Hamiltonian path
 *          validity verification against the raw input matrix (permutation,
 *          precedence constraints, and exact cost recomputation from scratch)
 *          and CSV output formatting for stdout and optional append-mode files.
 */

#ifndef SOP_VALIDATOR_HPP
#define SOP_VALIDATOR_HPP

#include "types.hpp"
#include <string>

namespace sop {

class Validator {
public:
    // Independently verifies that `res.best_path` is a valid permutation of 0..N-1,
    // satisfies all precedence constraints T[i][j] < 0 in `inst.raw_matrix`, and
    // has exact consecutive edge sum equal to `res.best_cost`.
    static bool validate_solution(const SopInstance& inst,
                                  const SolverResult& res,
                                  std::string& error_msg);

    // Formats and writes the CSV header and result row to stdout and (if specified)
    // appends to `opts.output_csv_path`.
    static void write_csv_output(const SolverOptions& opts, const SolverResult& res);
};

} // namespace sop

#endif // SOP_VALIDATOR_HPP

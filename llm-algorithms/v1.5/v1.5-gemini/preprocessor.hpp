/*
 * File: preprocessor.hpp
 * Purpose: Declares the SOP preprocessing pipeline: dynamic start/end node
 *          derivation, Warshall bit-parallel transitive closure, cycle
 *          detection, Escudero arc elimination, position-window tightening,
 *          and forward/backward order-ideal symmetry probing.
 */

#ifndef SOP_PREPROCESSOR_HPP
#define SOP_PREPROCESSOR_HPP

#include "types.hpp"

namespace sop {

class Preprocessor {
public:
    // Executes the full preprocessing pipeline on `inst` in-place.
    static void run(SopInstance& inst);

private:
    static void build_initial_precedences(SopInstance& inst);
    static void derive_start_and_end_nodes(SopInstance& inst);
    static bool compute_transitive_closure(SopInstance& inst);
    static void eliminate_infeasible_arcs(SopInstance& inst);
    static void compute_position_windows(SopInstance& inst);
    static bool should_transpose_poset(const SopInstance& inst);
    static void transpose_instance(SopInstance& inst);
};

} // namespace sop

#endif // SOP_PREPROCESSOR_HPP

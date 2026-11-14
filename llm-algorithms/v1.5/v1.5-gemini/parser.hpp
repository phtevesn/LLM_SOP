/*
 * File: parser.hpp
 * Purpose: Declares the TSPLIB-style .sop file parser supporting arbitrary line
 *          wrapping, keyword metadata headers, and optional EOF terminators.
 */

#ifndef SOP_PARSER_HPP
#define SOP_PARSER_HPP

#include "types.hpp"
#include <string>

namespace sop {

class SopParser {
public:
    // Parses a TSPLIB-style .sop file from disk into a SopInstance structure.
    // Throws std::runtime_error if the file cannot be opened or is malformed.
    static SopInstance parse(const std::string& filepath);
};

} // namespace sop

#endif // SOP_PARSER_HPP

/*
 * File: parser.cpp
 * Purpose: Implements a state-machine parser for TSPLIB-style Sequential Ordering
 *          Problem (.sop) files. Parses header keywords (NAME, TYPE, COMMENT,
 *          DIMENSION, EDGE_WEIGHT_TYPE, EDGE_WEIGHT_FORMAT) and reads the N x N
 *          FULL_MATRIX after EDGE_WEIGHT_SECTION across arbitrary line breaks.
 */

#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace sop {

namespace {

inline std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

} // namespace

/*
 * Algorithm: Two-Stage Keyword & Token-Stream TSPLIB Matrix Parser
 * Part: Stage 1 parses colon-delimited metadata headers line-by-line until
 *       EDGE_WEIGHT_SECTION; Stage 2 streams integer tokens across arbitrary
 *       line wraps, consumes the repeated dimension token N if present, and
 *       reads exactly N * N matrix entries without relying on an EOF marker.
 */
SopInstance SopParser::parse(const std::string& filepath) {
    std::ifstream in(filepath);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open input SOP file: " + filepath);
    }

    SopInstance inst;
    inst.filename = std::filesystem::path(filepath).filename().string();
    inst.name = inst.filename;

    std::string line;
    bool in_matrix_section = false;

    while (std::getline(in, line)) {
        std::string tline = trim(line);
        if (tline.empty()) continue;

        if (tline.rfind("EDGE_WEIGHT_SECTION", 0) == 0) {
            in_matrix_section = true;
            break;
        }
        if (tline == "EOF") {
            break;
        }

        auto colon_pos = tline.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = trim(tline.substr(0, colon_pos));
            std::string val = trim(tline.substr(colon_pos + 1));
            if (key == "NAME") {
                if (!val.empty()) inst.name = val;
            } else if (key == "TYPE") {
                inst.type = val;
            } else if (key == "COMMENT") {
                inst.comment = val;
            } else if (key == "DIMENSION") {
                inst.n = std::stoi(val);
            }
        }
    }

    if (!in_matrix_section) {
        throw std::runtime_error("Missing EDGE_WEIGHT_SECTION in SOP file: " + filepath);
    }

    std::vector<int64_t> tokens;
    tokens.reserve(static_cast<size_t>(std::max(1, inst.n)) * std::max(1, inst.n) + 2);

    std::string tok;
    while (in >> tok) {
        if (tok == "EOF") break;
        char* endptr = nullptr;
        long long v = std::strtoll(tok.c_str(), &endptr, 10);
        if (endptr != tok.c_str() && *endptr == '\0') {
            tokens.push_back(static_cast<int64_t>(v));
            if (inst.n > 0 &&
                tokens.size() >= static_cast<size_t>(inst.n) * static_cast<size_t>(inst.n) + 1) {
                break;
            }
        }
    }

    if (inst.n <= 0 && !tokens.empty()) {
        inst.n = static_cast<int>(tokens[0]);
    }
    if (inst.n <= 0 || inst.n > MAX_SOP_N) {
        throw std::runtime_error("Invalid or unsupported DIMENSION N=" + std::to_string(inst.n));
    }

    const size_t expected_entries = static_cast<size_t>(inst.n) * static_cast<size_t>(inst.n);
    size_t offset = 0;
    if (tokens.size() >= expected_entries + 1 && tokens[0] == inst.n) {
        offset = 1;
    }
    if (tokens.size() < offset + expected_entries) {
        throw std::runtime_error("Insufficient matrix entries in SOP file: expected " +
                                 std::to_string(expected_entries) + ", got " +
                                 std::to_string(tokens.size() - offset));
    }

    inst.num_words = (inst.n + 63) / 64;
    inst.raw_matrix.assign(inst.n, std::vector<int64_t>(inst.n, 0));
    for (int i = 0; i < inst.n; ++i) {
        for (int j = 0; j < inst.n; ++j) {
            inst.raw_matrix[i][j] = tokens[offset + static_cast<size_t>(i) * inst.n + j];
        }
    }

    return inst;
}

} // namespace sop

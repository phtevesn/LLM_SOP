#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

/*
 * Validate only SOP precedence constraints.
 *
 * Usage:
 *   validate_precedence instance.sop 0 11 4 13 ...
 *   validate_precedence instance.sop "0 11 4 13 ..."
 *
 * For an SOP matrix, a negative T[i][j] means that j must appear before i.
 */

static bool read_sop_matrix(const std::string &filename,
                            std::vector<std::vector<int>> &matrix)
{
    std::ifstream input(filename);
    if (!input) {
        std::cerr << "Could not open SOP file: " << filename << '\n';
        return false;
    }

    std::string line;
    bool in_edge_weight_section = false;
    std::vector<int> values;

    while (std::getline(input, line)) {
        if (line == "EDGE_WEIGHT_SECTION") {
            in_edge_weight_section = true;
            continue;
        }
        if (line == "EOF")
            break;
        if (!in_edge_weight_section)
            continue;

        std::istringstream line_stream(line);
        int value;
        while (line_stream >> value)
            values.push_back(value);
    }

    if (values.empty()) {
        std::cerr << "No matrix found in SOP file.\n";
        return false;
    }

    const int dimension = values.front();
    if (dimension <= 0) {
        std::cerr << "Invalid SOP dimension: " << dimension << '\n';
        return false;
    }

    const std::size_t expected_values =
        1u + static_cast<std::size_t>(dimension) * dimension;
    if (values.size() < expected_values) {
        std::cerr << "Incomplete SOP matrix: expected " << dimension * dimension
                  << " entries, found " << values.size() - 1 << '\n';
        return false;
    }

    matrix.assign(dimension, std::vector<int>(dimension));
    std::size_t offset = 1;
    for (int row = 0; row < dimension; ++row) {
        for (int column = 0; column < dimension; ++column)
            matrix[row][column] = values[offset++];
    }
    return true;
}

static bool read_path(int argc, char **argv, std::vector<int> &path)
{
    for (int argument = 2; argument < argc; ++argument) {
        std::istringstream path_stream(argv[argument]);
        int node;
        while (path_stream >> node)
            path.push_back(node);

        if (!path_stream.eof()) {
            std::cerr << "Invalid path value: " << argv[argument] << '\n';
            return false;
        }
    }
    return !path.empty();
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " instance.sop \"node0 node1 ...\"\n";
        return 2;
    }

    std::vector<std::vector<int>> matrix;
    std::vector<int> path;
    if (!read_sop_matrix(argv[1], matrix) || !read_path(argc, argv, path))
        return 2;

    std::vector<int> position(matrix.size(), -1);
    bool valid_node_ids = true;
    for (std::size_t index = 0; index < path.size(); ++index) {
        const int node = path[index];
        if (node < 0 || node >= static_cast<int>(matrix.size())) {
            std::cerr << "Path contains node outside SOP range: " << node << '\n';
            valid_node_ids = false;
            continue;
        }
        if (position[node] == -1)
            position[node] = static_cast<int>(index);
    }
    if (!valid_node_ids)
        return 1;

    bool valid = true;
    int violations = 0;
    int missing_endpoints = 0;

    for (int dependent = 0; dependent < static_cast<int>(matrix.size()); ++dependent) {
        for (int prerequisite = 0;
             prerequisite < static_cast<int>(matrix.size()); ++prerequisite) {
            if (matrix[dependent][prerequisite] >= 0)
                continue;

            if (position[dependent] == -1 || position[prerequisite] == -1) {
                std::cout << "UNVERIFIED: constraint " << prerequisite << " -> "
                          << dependent << " has a missing node in the path\n";
                valid = false;
                ++missing_endpoints;
            } else if (position[prerequisite] >= position[dependent]) {
                std::cout << "VIOLATION: node " << prerequisite
                          << " must appear before node " << dependent << '\n';
                valid = false;
                ++violations;
            }
        }
    }

    if (valid) {
        std::cout << "VALID: all precedence constraints are satisfied.\n";
        return 0;
    }

    std::cout << "INVALID: " << violations << " precedence violation(s), "
              << missing_endpoints << " constraint(s) could not be verified.\n";
    return 1;
}

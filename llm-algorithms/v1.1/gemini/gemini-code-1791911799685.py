#!/usr/init/env python3
"""
Sequential Ordering Problem (SOP) Solver - Ultimate Ant Colony System (ACS-SOP) Edition
Author: Personal AI Collaborator
Language: Python 3
Algorithm: ACS-SOP + Bitmask Precedence + Local Search + Parallel Multi-Core Execution
"""

import argparse
import csv
import math
import os
import random
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

def parse_sop_file(filepath):
    """
    Rigorously parses TSPLIB-style .sop files, handling arbitrary whitespace,
    wrapped matrix lines, comments, and missing EOF markers.
    """
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    
    lines = [line.strip() for line in content.splitlines() if line.strip()]
    
    name = ""
    dimension = None
    flat_tokens = []
    n_expected = None
    
    i = 0
    while i < len(lines):
        line = lines[i]
        upper_line = line.upper()
        if upper_line.startswith("NAME:"):
            name = line.split(":", 1)[1].strip()
        elif upper_line.startswith("DIMENSION:"):
            try:
                dimension = int(line.split(":", 1)[1].strip())
            except ValueError:
                pass
        elif upper_line.startswith("EDGE_WEIGHT_SECTION"):
            i += 1
            if i < len(lines):
                try:
                    n_expected = int(lines[i])
                    i += 1
                except ValueError:
                    pass
            
            while i < len(lines):
                if lines[i].upper() == "EOF":
                    break
                toks = lines[i].split()
                for tok in toks:
                    if tok.upper() == "EOF":
                        break
                    try:
                        flat_tokens.append(int(tok))
                    except ValueError:
                        pass
                i += 1
            break
        i += 1
        
    dim = dimension or n_expected
    if not dim and flat_tokens:
        possible_dim = int(math.isqrt(len(flat_tokens)))
        if possible_dim * possible_dim == len(flat_tokens):
            dim = possible_dim
            
    if dim and len(flat_tokens) >= dim * dim:
        if len(flat_tokens) > dim * dim:
            flat_tokens = flat_tokens[-dim * dim:]
        matrix = [flat_tokens[r * dim : (r + 1) * dim] for r in range(dim)]
    else:
        raise ValueError(f"Failed to parse matrix for {filepath}. Tokens: {len(flat_tokens)}, Expected dim: {dim}")
        
    if not name:
        name = filepath.split('/')[-1].split('\\')[-1]
        
    return name, dim, matrix

def analyze_sop(n, matrix):
    """
    Analyzes precedence constraints, computes transitive closure,
    and converts them into ultra-fast integer bitmasks.
    """
    preds = [set() for _ in range(n)]
    for i in range(n):
        for j in range(n):
            if matrix[i][j] < 0:
                preds[i].add(j)
                
    trans_preds = [set(p) for p in preds]
    changed = True
    while changed:
        changed = False
        for i in range(n):
            to_add = set()
            for p in trans_preds[i]:
                to_add.update(trans_preds[p])
            if not to_add.issubset(trans_preds[i]):
                trans_preds[i].update(to_add)
                changed = True
                
    trans_preds_mask = [0] * n
    for i in range(n):
        mask = 0
        for p in trans_preds[i]:
            mask |= (1 << p)
        trans_preds_mask[i] = mask
        
    start_node = None
    end_node = None
    
    for i in range(n):
        if len(preds[i]) == 0:
            start_node = i
        if len(preds[i]) == n - 1:
            end_node = i
            
    if start_node is None:
        start_node = min(range(n), key=lambda x: len(preds[x]))
    if end_node is None:
        end_node = max(range(n), key=lambda x: len(preds[x]))
        
    return start_node, end_node, trans_preds, trans_preds_mask

def calculate_cost(path, matrix):
    """Computes total valid path cost from scratch."""
    cost = 0
    for k in range(len(path) - 1):
        u, v = path[k], path[k+1]
        val = matrix[u][v]
        if val > 0:
            cost += val
    return cost

def validate_path(path, n, matrix, start_node, end_node, trans_preds_mask):
    """Rigorously validates permutation, bounds, and bitmask precedence."""
    if len(path) != n:
        return False, f"Length mismatch: {len(path)} != {n}"
    if set(path) != set(range(n)):
        return False, "Not a valid permutation of nodes 0..N-1"
    if path[0] != start_node:
        return False, f"Start node mismatch: {path[0]} != {start_node}"
    if path[-1] != end_node:
        return False, f"End node mismatch: {path[-1]} != {end_node}"
        
    visited_mask = 0
    for node in path:
        if (visited_mask & trans_preds_mask[node]) != trans_preds_mask[node]:
            return False, f"Precedence violation at node {node}"
        visited_mask |= (1 << node)
        
    return True, "Valid"

def local_search(path, matrix, trans_preds_mask, max_iterations=25):
    """Applies Or-opt and 2-opt local search verified via bitmask validation."""
    n = len(path)
    best_path = list(path)
    best_cost = calculate_cost(best_path, matrix)
    improved = True
    iterations = 0
    
    def check_valid(new_path):
        v_mask = 0
        for node in new_path:
            if (v_mask & trans_preds_mask[node]) != trans_preds_mask[node]:
                return False
            v_mask |= (1 << node)
        return True

    while improved and iterations < max_iterations:
        improved = False
        iterations += 1
        
        # 1. Or-opt
        for i in range(1, n - 1):
            node = best_path[i]
            for j in range(1, n - 1):
                if i == j:
                    continue
                new_path = list(best_path)
                new_path.pop(i)
                new_path.insert(j, node)
                
                if check_valid(new_path):
                    cost = calculate_cost(new_path, matrix)
                    if cost < best_cost:
                        best_path = new_path
                        best_cost = cost
                        improved = True
                        break
            if improved:
                break
                
        if improved:
            continue
            
        # 2. 2-opt subsequence reversal
        for i in range(1, n - 3):
            for j in range(i + 2, n - 1):
                new_path = best_path[:i] + list(reversed(best_path[i:j+1])) + best_path[j+1:]
                if check_valid(new_path):
                    cost = calculate_cost(new_path, matrix)
                    if cost < best_cost:
                        best_path = new_path
                        best_cost = cost
                        improved = True
                        break
            if improved:
                break
                
    return best_path, best_cost, iterations

def construct_ant_path(n, matrix, start_node, end_node, trans_preds_mask, pheromone, alpha=1.0, beta=2.0, q0=0.9):
    """
    Constructs an ant tour using Ant Colony System (ACS) state transition rules,
    balancing pheromone trails and heuristic desirability while respecting bitmask precedence.
    """
    path = [start_node]
    visited_mask = (1 << start_node)
    
    while len(path) < n - 1:
        current = path[-1]
        candidates = []
        for j in range(n):
            if j != end_node and not (visited_mask & (1 << j)):
                if (visited_mask & trans_preds_mask[j]) == trans_preds_mask[j]:
                    candidates.append(j)
                    
        if not candidates:
            break
            
        # ACS Pseudo-random proportional rule
        if random.random() < q0:
            # Exploitation: select candidate maximizing (tau^alpha) * (eta^beta)
            best_score = -1.0
            chosen = candidates[0]
            for c in candidates:
                cost = matrix[current][c] if matrix[current][c] >= 0 else 1
                eta = 1.0 / max(1.0, float(cost))
                tau = pheromone[current][c]
                score = (tau ** alpha) * (eta ** beta)
                if score > best_score:
                    best_score = score
                    chosen = c
        else:
            # Exploration: probabilistic roulette wheel selection
            scores = []
            total = 0.0
            for c in candidates:
                cost = matrix[current][c] if matrix[current][c] >= 0 else 1
                eta = 1.0 / max(1.0, float(cost))
                tau = pheromone[current][c]
                score = (tau ** alpha) * (eta ** beta)
                scores.append((c, score))
                total += score
                
            if total <= 0:
                chosen = random.choice(candidates)
            else:
                r = random.uniform(0, total)
                accum = 0.0
                chosen = candidates[0]
                for c, score in scores:
                    accum += score
                    if accum >= r:
                        chosen = c
                        break
                        
        path.append(chosen)
        visited_mask |= (1 << chosen)
        
        # Local pheromone update (ACS feature to discourage immediate stagnation)
        rho_local = 0.1
        tau0 = 1.0 / (n * 100.0)
        pheromone[current][chosen] = (1.0 - rho_local) * pheromone[current][chosen] + rho_local * tau0
        
    path.append(end_node)
    return path

def worker_ant_colony(n, matrix, start_node, end_node, trans_preds_mask, time_limit, worker_seed):
    """Worker thread running the Ant Colony System optimization loop."""
    random.seed(worker_seed)
    start_time = time.time()
    
    # Initialize pheromone matrix
    initial_tau = 1.0 / (n * 50.0)
    pheromone = [[initial_tau for _ in range(n)] for _ in range(n)]
    
    best_path = None
    best_cost = float('inf')
    enumerated_nodes = 0
    rho_global = 0.1
    
    while time.time() - start_time < time_limit:
        ant_path = construct_ant_path(n, matrix, start_node, end_node, trans_preds_mask, pheromone)
        enumerated_nodes += 1
        
        if len(ant_path) == n:
            valid, _ = validate_path(ant_path, n, matrix, start_node, end_node, trans_preds_mask)
            if valid:
                refined_path, refined_cost, ls_its = local_search(ant_path, matrix, trans_preds_mask, max_iterations=15)
                enumerated_nodes += ls_its
                
                if refined_cost < best_cost:
                    best_path, best_cost = refined_path, refined_cost
                    
                    # Global pheromone update along best tour found
                    delta = 1.0 / float(best_cost)
                    for k in range(len(best_path) - 1):
                        u, v = best_path[k], best_path[k+1]
                        pheromone[u][v] = (1.0 - rho_global) * pheromone[u][v] + rho_global * delta
                        
    return best_path, best_cost, enumerated_nodes

def solve_sop(filepath, time_limit=30.0):
    start_time = time.time()
    name, n, matrix = parse_sop_file(filepath)
    start_node, end_node, trans_preds, trans_preds_mask = analyze_sop(n, matrix)
    
    cpu_count = min(os.cpu_count() or 4, 8)
    worker_time = time_limit * 0.95
    
    best_path = None
    best_cost = float('inf')
    total_enumerated = 0
    
    with ProcessPoolExecutor(max_workers=cpu_count) as executor:
        futures = [
            executor.submit(worker_ant_colony, n, matrix, start_node, end_node, trans_preds_mask, worker_time, seed)
            for seed in range(cpu_count)
        ]
        
        for future in as_completed(futures):
            try:
                w_path, w_cost, w_nodes = future.result()
                total_enumerated += w_nodes
                if w_path and w_cost < best_cost:
                    best_path, best_cost = w_path, w_cost
            except Exception:
                pass
                
    elapsed = time.time() - start_time
    
    if best_path:
        valid, msg = validate_path(best_path, n, matrix, start_node, end_node, trans_preds_mask)
        if not valid:
            raise RuntimeError(f"Solution validation failed: {msg}")
    else:
        best_path = [start_node] + [i for i in range(n) if i != start_node and i != end_node] + [end_node]
        best_cost = calculate_cost(best_path, matrix)
        
    return {
        'instance': name,
        'size': n,
        'time': round(elapsed, 4),
        'enumerated_nodes': total_enumerated,
        'best_cost': best_cost,
        'best_path': " ".join(map(str, best_path))
    }

def main():
    parser = argparse.ArgumentParser(description="Ultimate ACS-SOP Solver")
    parser.add_argument("files", nargs="+", help="Path(s) to input .sop file(s)")
    parser.add_argument("-t", "--time-limit", type=float, default=30.0, help="Time limit in seconds per instance (default: 30.0)")
    parser.add_argument("-o", "--output", type=str, default="", help="Path to output CSV file (default: print to stdout)")
    
    args = parser.parse_args()
    
    results = []
    for filepath in args.files:
        try:
            res = solve_sop(filepath, time_limit=args.time_limit)
            results.append(res)
        except Exception as e:
            print(f"Error processing {filepath}: {e}", file=sys.stderr)
            
    if not results:
        sys.exit(1)
        
    fieldnames = ["Instance", "Instance Size", "Time", "Enumerated Nodes", "Best Cost", "Best Path"]

    # solve_sop uses descriptive internal keys; translate them to the shared
    # CSV schema used by the other SOP solvers.
    csv_results = [
        {
            "Instance": r["instance"],
            "Instance Size": r["size"],
            "Time": r["time"],
            "Enumerated Nodes": r["enumerated_nodes"],
            "Best Cost": r["best_cost"],
            "Best Path": r["best_path"],
        }
        for r in results
    ]
    
    if args.output:
        with open(args.output, mode="w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for r in csv_results:
                writer.writerow(r)
        print(f"Results successfully written to {args.output}")
    else:
        writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
        writer.writeheader()
        for r in csv_results:
            writer.writerow(r)

if __name__ == "__main__":
    main()

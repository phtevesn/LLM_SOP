# Sequential Ordering Problem (SOP) Solver

Strongest solver that can be implemented in the current environment
(Python + PuLP/CBC).

## Algorithm

1. **Transitive closure** of the precedence DAG  
2. **Forced-arc detection** – arcs that must appear in every feasible path are fixed  
3. **Arc filtering** – illegal arcs removed before the MIP is built  
4. **Multi-start constructive heuristic** + **2-opt** + **Or-opt** local search  
   (gives a strong warm-start)  
5. **Tight MIP** solved by multi-threaded CBC:
   - Binary arc variables (only legal arcs)
   - Exactly `N-1` arcs
   - Miller-Tucker-Zemlin positions
   - Explicit start/end indicators
   - Transitively closed precedence constraints
   - Forced arcs fixed to 1
   - Warm-start from the heuristic
6. CBC log parsed for **Enumerated Nodes** and **gap**

## Requirements

- Python 3.8+
- PuLP (`pip install pulp`) – CBC is bundled

## Usage

```bash
python sop_solver.py INSTANCE1.sop [INSTANCE2.sop ...] [--time SECONDS] [--out RESULT.csv]
```

## Example (prob.42.sop)

```
prob.42.sop: N=42  cost=251  nodes=...  time=...s  [optimal]
```

## Input / Output

Standard TSPLIB SOP format.  
CSV columns: `Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path`

When the solution is not proven optimal the relative gap (if reported by CBC) is printed on stderr.

## Validity

Every returned path is fully re-validated.

## Limitations

- Excellent for N ≲ 60–70 on typical densities.
- Extremely sparse large instances may still benefit from a specialised dual bound or a commercial solver.
- Or-opt is a good neighbourhood; a full SOP-3-exchange would be even stronger but significantly more code.

## License

Public domain / CC0.

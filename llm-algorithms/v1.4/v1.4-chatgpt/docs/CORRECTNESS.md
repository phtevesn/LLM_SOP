# Correctness and proof obligations

This document explains why each pruning rule preserves an optimum. It is an
algorithmic argument, not a formal verification of the C++ implementation.
`tests/test_assignment.cpp` and `tests/test_solver.py` independently exercise
these obligations. Final path validation proves feasibility and cost, while
optimality additionally depends on the exact search and its sound pruning rules.

## 1. Feasibility and preprocessing

For each negative off-diagonal entry `T[i][j]`, add the directed precedence edge
`j -> i`. A directed cycle makes the instance infeasible. Otherwise every
topological ordering is a feasible Hamiltonian path in the supplied complete
matrix: a negative consecutive transition would imply the opposite ordering
and contradict topological validity.

The predecessor closure is computed by propagating predecessor sets along a
topological ordering. Arc `i -> j` can be removed when `j` precedes `i`.
It can also be removed when some `k` satisfies `i < k < j` in the partial order,
because `i` and `j` cannot be consecutive in any valid ordering. A vertex with
no predecessor can be a start; one with no successor can be an end. The dummy
vertex has zero-cost arcs to every possible start and from every possible end.
No valid endpoint choice is excluded.

## 2. The residual assignment lower bound

Fix a valid prefix of cost `g`, ending at `a`; use dummy `d` for an empty prefix.
Let `U` be the unvisited vertices. Residual rows are `{a} union U`, columns are
`U union {d}`. A full feasible suffix induces one outgoing edge per row and
one incoming edge per column, hence a perfect matching. The relaxation permits
cycles in `U`, so its optimum `A` satisfies `g + A <= optimum extension cost`.

The frontier row may only go to a currently eligible vertex. It can go directly
to the dummy only when `U` is empty. All other rows use the preprocessed static
adjacency matrix. These restrictions hold for every feasible suffix.

The Hungarian solver maintains integral duals `u[r], v[c]` satisfying
`u[r] + v[c] <= cost[r,c]` for every finite residual arc. Matched arcs are tight.
The matching's cost equals the sum of active row and column duals, establishing
optimality of the assignment without floating-point tolerances.

Appending `b` fixes arc `a -> b`, removes row `a` and column `b`, and makes row
`b` the new frontier. Deleting a row/column leaves at most one unmatched row;
new frontier restrictions can unmatch one more. The removed frontier is the
only previously restricted row, so every surviving old dual constraint either
stays unchanged or becomes less restrictive to satisfy because an arc is
removed. At most two shortest augmentations restore an optimal matching.

For a candidate next vertex `b`, any completion must use arc `a -> b`, whose
nonnegative reduced cost is `rc = cost[a,b] - u[a] - v[b]`. Therefore
`g + A + rc` is a lower bound on that child before repairing its assignment.
If it is at least the incumbent, the child cannot improve the answer.

## 3. Connecting matching components

The residual matching decomposes into one directed frontier-to-dummy path
and disjoint cycles. For any perfect matching `M`,

`cost(M) = A + sum(reduced_cost(e) for e in M)`.

All reduced costs are nonnegative. Contract the path and each cycle into
components. Each cycle needs at least one outgoing crossing arc and one incoming
crossing arc in a Hamiltonian completion. Summing the cheapest outgoing arc
for each cycle counts different source-component edges, so it is a lower bound
on the required reduced cost. Summing cheapest incoming arcs has the analogous
argument. The undirected crossing graph must also be connected, so a minimum
spanning tree using the cheaper directed crossing weight is another lower bound.

Take the maximum of these three penalties. Adding them would generally count
some arcs multiple times and is not valid. The candidate bound in Section 2
uses `A`, not `A + component penalty`, for the same reason. Deadline cancellation
during this optional calculation returns a weaker zero penalty and never a proof.

## 4. History domination

Two valid prefixes with the same visited set `S` and last vertex `a` have
identical available suffixes and identical suffix costs. If their costs are
`g1 <= g2`, every extension of the second is no cheaper than the corresponding
extension of the first. The second state can be discarded.

The table compares the complete bitset and last vertex under a shard mutex.
Hash collisions are not treated as equality. Cache replacement only forgets
a useful fact; it never creates one.

An in-progress representative can dominate another state. The representative
continues normal enumeration, is safely bound-pruned, or is itself dominated
by a no-more-expensive representative. Equal-cost arrivals cannot mutually
discard each other: the first successful insertion remains responsible for
exploration. Strict improvements cannot form a circular dependence with equal
cost. A descendant cannot have the same visited set as its ancestor. There is
no speculative thread restart that abandons the responsible representative.

If cancellation interrupts any representative, the whole run remains unproven.
An internal exception cancels the run and results in an error rather than a
successful optimality claim.

## 5. Upper bounds

Greedy construction, perturbation and local search only propose paths. Before
updating the pruning upper bound, the solver independently checks the original
matrix: path length, index range, unique membership, every precedence relation,
and recomputed cost. Therefore the shared incumbent is always a feasible upper
bound. It decreases monotonically under a mutex and is published atomically.

An assignment solution that forms one path is also checked for precedence;
connectivity alone does not make it a feasible SOP solution.

## 6. Enumeration and parallel completion

At any valid prefix, every feasible extension starts with an unvisited vertex
whose predecessors are all visited. The search branches over every such next
vertex, except children eliminated by sound lower bounds or history domination.
The tree is finite, with depth at most `N`.

Each queued or executing task contributes to one outstanding-task counter.
A donor increments the counter while it still owns its active task, before
making the new task visible. Moving a task from a queue to a worker does not
decrement the counter. Finishing a task decrements it exactly once. Thus zero
means that no queued or active exact task remains. Empty individual queues
are not a proof condition.

The incumbent is optimal when either:

1. Its cost equals the admissible root lower bound; or
2. Every exact task has finished and every potentially improving extension has
   been exhausted or soundly pruned.

The proof is committed only while the shared deadline/cancellation predicate
is false. The final `Optimal=yes` additionally requires a nonempty valid path.
Cleanup after a completed proof does not invalidate it. Infeasibility is
reported separately and never represented by an empty "optimal path".

## 7. Integer and resource behavior

Input costs are bounded conservatively relative to `N` and signed 64-bit range.
Negative entries are never added to objective costs. Internal `INF` arcs are
skipped before reduced-cost arithmetic. The component penalties use a maximum,
and all counted cost additions correspond to bounded feasible objectives or
dual distances. No floating-point quantity participates in correctness pruning.

The cache has a fixed configured capacity. A failed cache allocation disables
that shard, retaining exactness. Other allocation failures or thread creation
failures are fatal diagnostics; no proof or result row is manufactured from an
incomplete failed execution. OS termination such as SIGKILL is outside the
cooperative cancellation mechanism.

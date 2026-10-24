# Working notes (established / ruled out / open)

## Established (each backed by an observed run)
- Reference solvers (brute force, bitmask DP) agree on 600/600 random instances.
- Solver == reference on: 250/250 (N<=8, 1 thread and 4 oversubscribed threads), 150/150 (N<=13), 60/60 + 60/60 (N 14-24 vs tools/dp_ref).
- Mutants SOP_MUTANT=1,2,3 are caught (81 / 3 / 81 failures of 400). Mutant 2 (history off-by-one) is the weakest detector.
- TSan / ASan / UBSan: no reports (small instances; 30-40 s on prob_42 / rbg048a with 4 threads).
- prob_42 -> 243 proven (97-102 s, 31,989,350 nodes); R_200_1000_1 -> 1404 proven (37-41 s, 2,005,643 nodes); rbg048a -> 351 found at 0.2 s, not proven in 120 s (88.9M nodes).
- Root AP bounds: prob_42 174 (opt 243), R_200 1392 (opt 1404), rbg048a 337 (best 351).
- On R_200 the ILS reaches only ~3097; B&B dive finds 1404 at ~7.7 s; proof then takes ~30 s.

## Ruled out
- Plain layered DP as ground truth for prob_42 / rbg048a: >8M states by layer 7-8 (gives up).
- Bounded DP with trivial min-out-arc bound as independent refutation: >15M states by layer 8.
- Suffix-reuse ("history utilization") in parallel: unsound with in-progress entries -> not implemented.

## Open
- No independent confirmation of the three real-instance results (published 243 / 351 recalled from memory, unchecked).
- rbg048a proof: 1500 s run was killed by a sandbox VM restart (uptime showed 0 min); outcome unknown.
- 32-core scaling, NUMA pinning / mbind / matrix replicas: never run on real multi-core/multi-node hardware.
- Behaviour at N ~ 1000 untested.
- ILS is weak on random asymmetric costs.

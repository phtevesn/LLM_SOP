Claude SOP solvers
==================

Run these commands from this directory:

    cd /home/researchers/steveninouye/algorithms/LLM_SOP/llm-algorithms/claude-iter-1

1. Convert an SOP file for the C++ heuristic
---------------------------------------------

`make_inst.py` reads a TSPLIB-style `.sop` file and writes `inst.txt`, which is
the intermediate format expected by `ils.cpp`:

    python3 make_inst.py ../../tsplib/prob.42.sop

The generated `inst.txt` contains the dimension followed by the full matrix.
It is overwritten each time `make_inst.py` runs.

2. Build and run the iterated local search
-------------------------------------------

Build the C++ heuristic once:

    g++ -O2 -o ils ils.cpp

Then run it with a time limit in seconds:

    ./ils 30

`ils.cpp` reads `inst.txt` from the current directory. The output is a
heuristic result printed to the terminal, for example:

    BEST 251 time_to_best 2.314 iters 1234 evals 56789 valid 1
    0 11 4 13 ... 41

3. Run branch-and-bound
-----------------------

`bb.py` accepts an initial upper-bound cost and the original `.sop` file:

    python3 bb.py 243 ../../tsplib/prob.42.sop

The default branch-and-bound time limit is one hour (3600 seconds). To use a
different limit, provide it as a fourth argument:

    python3 bb.py 243 ../../tsplib/prob.42.sop 600

The arguments are:

    python3 bb.py UPPER_BOUND INPUT.sop [TIME_LIMIT_SECONDS]

When the deadline is reached, the search stops and reports `RESULT TIME_LIMIT`.
Otherwise it reports `RESULT COMPLETE`. The upper bound should normally come
from the best cost found by `ils` or another heuristic.

`bb.py` requires NumPy and SciPy:

    python3 -m pip install numpy scipy

Input paths may be absolute or relative to the directory from which the
command is run. Python does not expand `~` automatically, so use an absolute
path or expand it in the calling script.

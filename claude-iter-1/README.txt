Run order (all files in one folder):
  python3 make_inst.py prob_42.sop     # writes inst.txt for the C++ heuristic
  g++ -O2 -o ils ils.cpp && ./ils 30   # iterated local search, 30 s time limit
  python3 bb.py 243                    # exact B&B; argument = upper bound (heuristic cost)
Notes: bb.py reads the .sop path hardcoded near the top (edit it); needs numpy + scipy.

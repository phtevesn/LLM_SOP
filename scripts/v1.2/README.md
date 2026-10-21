# Run the v1.2 solvers

From the repository root (Python 3.10+ and GCC 11+ required for ChatGPT's
C++20 source; Claude and Gemini are built as C++17):

```bash
python3 scripts/v1.2/run_all_llms.py --dry-run
python3 scripts/v1.2/run_all_llms.py
```

The runner compiles ChatGPT, Claude, and Gemini from `llm-algorithms/v1.2` and
runs each solver on every listed instance, sequentially. Each process receives
32 threads and a 3600-second time limit by default. Ten instances across three
solvers can take roughly 30 hours plus compilation and timeout grace periods.
The script can be launched from any directory; explicit relative CLI paths are
relative to the current working directory.

The default list is `results/instances.txt` if present, otherwise
`results/llm-env-resultsv1.2/instances.txt` (with a notice). Blank lines and `#`
comment lines are ignored. Relative entries resolve against the list directory,
then the repository root. The known `LLM_SOPtsplib`/`LLM_SOPsoplib` missing-slash
typos and missing `.sop` extensions are resolved with notices. Any unresolved
entry stops the entire run before compilation, so instances are never silently
skipped. The original list is left unchanged.

Each invocation creates a unique UTC-named `run-*` directory under
`results/llm-env-resultsv1.2`, containing:

- `chatgpt.csv`, `claude.csv`, `gemini.csv`: standard solver CSV rows from successful executions.
- `summary.csv`: status, exit code, process wall time, and result path or failure detail for each execution; build failures have a `<build>` entry.
- `instances.txt`: resolved input paths in execution order.
- Per-solver directories: binaries, compiler logs, individual CSVs, and separate stdout/stderr logs for each instance.

Results and summary rows are flushed after each instance. A failed execution
does not prevent subsequent runs. Exit status is 1 if any build or execution
fails, 2 for invalid arguments/inputs, and 130 for a keyboard interruption.
An ordinary solver time limit with a valid result row is successful even when
`Optimal=no`. A process still running after the limit plus 60 seconds is killed
and recorded as `external_timeout`; its partial output remains in its logs.
The runner checks CSV structure and instance identity, but does not independently
verify solution feasibility, costs, or optimality claims.

Custom settings:

```bash
python3 scripts/v1.2/run_all_llms.py --instances results/llm-env-resultsv1.2/instances.txt --threads 32 --time-limit 600
python3 scripts/v1.2/run_all_llms.py --claude-memory-mb 4096 --timeout-grace 120 --output-dir /tmp/sop-results
python3 scripts/v1.2/run_all_llms.py --compiler g++-11
```

Claude's transposition table budget defaults to 512 MiB in this runner, overriding
its automatic setting (which can reserve up to 16 GiB). This is not a total
process memory cap. ChatGPT and Gemini retain their solver memory defaults.
No solver sources are modified, and previous run directories are preserved.

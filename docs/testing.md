# Testing and whole-application coverage

The Task 7 coverage report measures every C file under `src/`. It does not
exclude any production file, line, or branch, including system-call error
branches. Test harness and instructor relay code are outside the production C
coverage scope.

| Source | Covered lines | Covered branches |
| --- | ---: | ---: |
| `src/lab.c` | 178/178 | 152/152 |
| `src/main.c` | 250/250 | 272/272 |
| Total | **428/428 (100%)** | **424/424 (100%)** |

## Reproduce

From the repository root, with GCC, GNU Make, Python 3.9+, and `gcovr` installed:

```bash
make all
make check
make report
make leak
make leak-test
```

For the assignment's clean-build check, run `make clean` before `make all`.
It removes generated files under `build`, including locally generated transfer
inputs/outputs. Saved evidence under `docs` is unaffected. An isolated fresh
build can instead use `BUILD_BASE_DIR=build/coverage-clean` on each command.

`make report` resets counters only in its three coverage build directories,
runs the test suites below, and combines their results. It fails unless both
line and branch coverage equal 100%. It takes about 90 seconds, including a
real 30-second receiver inactivity test. Do not run another coverage-writing
test command concurrently against the same build directory.

Reports are written under `build/report/` (or the chosen build base):

- `html/coverage_report.html`: annotated source and branch results.
- `txt/coverage_report.txt`: per-file line coverage.
- `coverage.json`: machine-readable line and branch counts.

## What runs

1. **22 protocol Unity tests** in `tests/lab-test.c` cover every public function
   in `src/lab.h`: checksum/wire fixtures, malformed packets, cumulative ACKs,
   window limits, retransmissions, FIN linger, time boundaries, file limits,
   and three seeded in-memory transfers with damage in both directions.
2. **12 adapter Unity tests** in `tests/adapter-test.c` exercise the actual
   `src/main.c` implementation. The harness includes that file in a separate
   translation unit with renamed system calls and application entry point.
   Deterministic substitutes inject interrupted/failed socket calls, resolver
   failures, clock/poll failures, allocation errors, short reads/writes, changing
   input, and failed closes. Assertions check outcomes, transmitted bytes,
   delivered payload, byte limits, retries, and resource cleanup. No test hooks
   or mock calls are compiled into the real application.
3. **14 real transfers and 11 CLI checks** run against the coverage-instrumented
   executable and the unmodified instructor `cs425_relay.py`. This verifies
   that the adapter and actual operating-system I/O work together.
4. **11 scripted I/O edge tests** run against the same real executable, including
   the actual 30-second inactivity deadline (`--slow`).

`make check` runs both Unity suites. `make leak-test` runs both with
AddressSanitizer, UndefinedBehaviorSanitizer, and leak detection. `make leak`
checks the application's no-argument usage path. `make integration` remains a
release-build transfer check with the ten default I/O edge tests.

The GitHub Actions coverage step and submission-report generator already invoke
`make report`, so they use this expanded scope. The existing compiler matrix
runs both unit suites and sanitizers for GCC and Clang; numeric coverage is
collected with GCC. Local validation does not establish that remote CI passed.

## Production changes

The CLI still accepts the same decimal rates: its explicit grammar guarantees
that `strtod` consumes a nonnegative decimal, so redundant conversion checks
were removed. The receiver state machine already enforces the 16 MiB limit;
the adapter now relies on that single check. A redundant timeout continuation
was removed because `wait_until` returns a timeout only after the deadline,
which the immediately following receiver tick handles. No packet format,
protocol timing, retry policy, command-line option, or exit code changed.

## Evidence

See `validation/coverage.log`, `validation/builds-tests-sanitizers.log`,
`validation/clean-build.log`, and `validation/coverage.txt`. The metadata in
`validation/environment.json` records source and test hashes. Task 6 benchmark
records remain separately in `benchmark/`; they are actual instructor-relay
measurements, not measurements of the coverage-instrumented executable.

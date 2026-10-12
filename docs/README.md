# Additional project documentation

Start with the root [README](../README.md) for prerequisites, compilation,
CLI examples, protocol design, testing, and performance results.

The supplied [P2 specification](../P2%20-%20Reliable%20Data%20Transfer.pdf)
defines the assignment. [onyx.md](onyx.md) contains the starter's Onyx access
instructions. `scripts/create-submission-report.sh` regenerates the submission
report after building, running unit tests, checking coverage, and running
sanitizers. Add `-m` when pandoc is unavailable.

The root `cs425_relay.py` is the instructor-provided relay and is the default
for local transfers, integration tests, and benchmarks. The older independent
`scripts/cs425_relay.py` remains available through `--relay` to reproduce the
compatibility-relay behavior. The current results in `benchmark/` and
`benchmark-results.md` were measured with the instructor relay.

[testing.md](testing.md) explains whole-application coverage, deterministic I/O
fault tests, real-relay tests, and the retained evidence in `validation/`.

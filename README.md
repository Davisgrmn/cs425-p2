# Project 2 - Reliable Data Transfer

- Name: Davis Garman
- Email: davisgarman@u.boisestate.edu
- Class: 425-001

## Overview

This C program transfers files up to 16 MiB over UDP using Go-Back-N through
the instructor's cs425_relay.py. UDP exposes loss for this assignment; TCP would
normally provide reliability and congestion control for production transfers.

## Building and Running

Requires Linux/WSL, a C11 compiler, GNU Make, and Python 3.9+. From the project
root, run make and create an input with cp README.md in.bin. Then use three
terminals, starting the receiver before the sender:

```bash
# Terminal 1
python3 cs425_relay.py --delay 50 --seed 7
# Terminal 2
./build/release/myapp recv -s davis-1 127.0.0.1 out.bin
# Terminal 3, within 30 seconds of starting the receiver
./build/release/myapp send -s davis-1 -w 16 -l 0.1 -c 0.05 127.0.0.1 in.bin &&
cmp in.bin out.bin && echo identical
```

Both endpoints use the same session. Restart the receiver for each transfer;
it replaces the output file. Defaults: window 8 (-w, range 1-64), timeout 250 ms
(-T), damage rates 0 (-l/-c/-d, decimal 0-0.5), port 4250 (-p). If occupied, set
--port on the relay and matching -p on both endpoints. Exit codes: 0 success,
1 invalid arguments, 2 I/O/transfer failure. No arguments prints usage, exiting 0.

## Design

1. Packets (src/lab.h and src/lab.c): pure checksum and encoding/decoding
   functions validate the 10-byte header, network byte order, and payload.
2. State machines (same files): process packets and injected timestamps,
   returning packets to send or bytes to deliver without sockets, files, or clocks.
3. I/O (src/main.c): handles getopt, getaddrinfo, registration, files, and one
   socket per endpoint, driving the protocol with poll and CLOCK_MONOTONIC.

This separation enables deterministic loss and timeout tests. The sender uses
cumulative ACKs and one timer, retransmitting its outstanding window on timeout.
The receiver writes in order and re-ACKs duplicates/gaps. FIN follows acknowledged
DATA; the receiver lingers two seconds to handle a lost final ACK. Registration
allows five attempts; sender failure follows ten timeouts without progress, and
receiver failure follows 30 seconds without valid traffic.

## Results

Task 6 used a random 1 MiB file, the relay with --delay 50, a 250 ms
timeout, and three trials per configuration. All 12 outputs passed cmp. Trials
ran concurrently with separate relays on Ubuntu/WSL2, GCC 13.3.0. GNU time
measured the sender, excluding receiver linger. Corruption/duplication were zero.

| Window | Loss | Mean seconds | Throughput (KiB/s) |
| ---: | ---: | ---: | ---: |
| 1 | 0 | 104.553 | 9.794 |
| 16 | 0 | 6.660 | 153.754 |
| 1 | 0.05 | 133.007 | 7.699 |
| 16 | 0.05 | 26.193 | 39.094 |

Throughput is 1024 / mean_seconds. Window 1's RTT is 104.553 / 1025 seconds =
102.003 ms. The extra 2.003 ms reflects scheduling, relay/socket processing,
timer resolution, and registration overhead. Window 16 needs about 64 DATA
round trips plus FIN: 65 * 102.003 ms = 6.630 s, close to 6.660 s measured.
Its 15.70x speedup falls below 16 because of FIN and startup/scheduling overhead.

Five percent loss increases time by 1.27x at window 1 and 3.93x at window 16.
Window 1 retransmits one packet; window 16 retransmits the outstanding window
while the receiver discards packets beyond the gap. Cumulative ACKs cover lost
ACKs but cannot repair missing DATA. Reproduce with make benchmark; raw records
are in docs/benchmark/ and full details in README.md.

## Tests and Coverage

Run make all check report leak leak-test. All local checks pass: 22 protocol
Unity tests, 12 adapter Unity tests, 14 instructor-relay transfers, 11 CLI cases,
and 11 I/O edge tests (including the real 30-second idle timeout). Unit tests
cover packet validation, ACKs/timeouts, file boundaries, and seeded transfers
with 20% damage rates in both directions. Both unit suites pass ASan/UBSan and
leak checks.

make report requires gcovr and enforces 100% line and branch coverage across
all production C files: 178 lines/152 branches in src/lab.c and 250 lines/272
branches in src/main.c (428 lines/424 branches total). No production files,
lines, or branches are excluded. The adapter tests substitute system calls to
exercise errors and cleanup; instrumented real transfers verify actual I/O.
The report resets counters each run and includes the slow timeout test, so
allow about 90 seconds. HTML: build/report/html/coverage_report.html.
Details and saved evidence: docs/testing.md and docs/validation/.

## Known Bugs or Issues

No failures in completed local tests. Timeouts above the two-second FIN linger
cannot recover a lost final ACK; use the default 250 ms.

## Experience

Separating protocol logic made damage tests repeatable; lost final ACKs showed
why linger matters. The relay rejected small rates formatted as
scientific notation. Preserving validated decimal text fixed that integration bug.

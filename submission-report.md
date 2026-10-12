# Submission Report

Generated: 2026-10-12 02:19:41 UTC

Machine: `Linux DGlenovo 6.6.87.2-microsoft-standard-WSL2 #1 SMP PREEMPT_DYNAMIC Thu Jun  5 18:30:46 UTC 2025 x86_64 x86_64 x86_64 GNU/Linux`

This report contains the README, actual build/test output, and project sources.

## README

# P2 — Reliable Data Transfer

CS425: reliable binary file transfer over UDP using Go-Back-N. One executable
provides sender and receiver modes. Files may be empty or up to 16 MiB.

## Compiling

Use Linux with a C11 compiler (GCC or Clang), GNU Make, and Python 3.8 or newer.
The C program uses POSIX sockets, `getopt`, `poll`, and
`clock_gettime(CLOCK_MONOTONIC)`; on Windows, build and run it inside WSL.
Run these commands from the repository root in a Linux/Bash terminal:

```bash
make                 # build/release/myapp
make all             # release, debug, coverage tests, sanitizer tests
./build/release/myapp # print usage; exits successfully
```

`make release CC=clang` selects Clang. Use a separate build directory when
switching compilers, for example `make all CC=clang BUILD_BASE_DIR=build-clang`.
The normal build has no third-party C dependencies.

In GitHub Codespaces, use the included C/C++ dev container or install
`build-essential`, `python3`, `python3-venv`, and `pandoc` if needed.
On Onyx, clone the repository and run the same `make all` command in its Linux
shell. The implementation uses standard POSIX interfaces and does not need
root privileges. Access information is in [docs/onyx.md](docs/onyx.md).

## Running a transfer

Open three terminals on the same Linux machine. The receiver must register
before the sender.

Terminal 1 — relay:

```bash
python3 scripts/cs425_relay.py --delay 50 --seed 7
```

Terminal 2 — receiver:

```bash
./build/release/myapp recv -s davis-1 127.0.0.1 out.bin
```

Terminal 3 — sender:

```bash
./build/release/myapp send -s davis-1 -w 16 -l 0.1 -c 0.05 127.0.0.1 in.bin
cmp in.bin out.bin && echo identical
```

The bundled relay is an independently implemented **compatibility relay** based
on the PDF. It is not the instructor's Canvas download. To use the official
relay, replace the first command with `python3 /path/to/cs425_relay.py --delay 50`.
Neither endpoint depends on the bundled relay's implementation.

If UDP port 4250 is occupied, choose a port between 20000 and 60000 and pass
`--port 24500` to the relay and `-p 24500` to both endpoints. A host name such as
`localhost` works as well as `127.0.0.1`. Each endpoint retains one socket for
registration and the entire transfer.

### Command line

```text
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]
                  [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
```

| Option | Meaning | Default / allowed values |
| --- | --- | --- |
| `-s` | Shared session name, required | 1–32 characters from `a-z`, `0-9`, `-` |
| `-w` | Sender window, in packets | 8; 1–64 |
| `-T` | Sender retransmission timeout, milliseconds | 250; 1–2147483647 |
| `-l` | Relay loss probability | 0; 0–0.5 |
| `-c` | Relay corruption probability | 0; 0–0.5 |
| `-d` | Relay duplication probability | 0; 0–0.5 |
| `-p` | Relay UDP port | 4250; 1–65535 |

Only `-s` and `-p` apply to the receiver. The receiver replaces the output file
after registration succeeds. An interrupted or failed transfer can leave a
partial output file.

Exit status is 0 for success or no-argument usage, 1 for an invalid command line,
and 2 for file/network errors, relay refusal, or a transfer timeout. Diagnostics
go to standard error. Registration waits one second per attempt, with at most
five HELLO attempts. The sender fails after ten consecutive retransmission
timeouts without an advancing ACK; the receiver fails after 30 seconds without
valid transfer traffic.

## Design

The implementation has three layers:

1. **Packets** in `src/lab.h` and `src/lab.c` compute the RFC 1071 Internet
   checksum and encode/decode the exact ten-byte header. Multi-byte fields use
   network byte order. Decode checks the datagram size, type, reserved byte,
   payload length, ACK/FIN semantics, and checksum before exposing any payload.
   Invalid datagrams are discarded without replying.
2. **Protocol state machines** in the same files accept packets and an injected
   millisecond timestamp. They return batches to transmit, payload to deliver,
   and ACK/finish actions. They never access sockets, files, or the real clock.
   Tests can therefore model loss by withholding a packet and model a timeout
   by advancing virtual time.
3. **I/O adapter** in `src/main.c` parses options with `getopt`, resolves the
   relay with `getaddrinfo`, registers, reads/writes files, and drives the state
   machines with `poll` and a monotonic clock. The IPv4 socket matches the
   assignment relay's loopback listener.

The sender tracks `base` and `next`, keeps a fixed ring of at most 64
unacknowledged packets, and runs one timer. A cumulative ACK advances `base`
and restarts the timer if packets remain outstanding. Duplicate ACKs leave it
alone. A timeout retransmits the entire outstanding window. The input is read
once into an immutable buffer, bounded at 16 MiB; retransmissions preserve the
original payload.

The receiver writes only the packet whose sequence number equals `expected`.
Other DATA packets are discarded and re-ACKed with the same next expected
sequence number. FIN is sent only after all DATA is acknowledged. An in-order
FIN closes the output before the final ACK; the receiver then lingers for a
fixed two seconds, answering repeated FINs without extending that deadline.
An empty file is FIN sequence 0 followed by ACK 1.

UDP makes packet loss visible to this implementation, so checksums, ordering,
ACKs, and retransmissions are built at the endpoints. A production file transfer
would normally use TCP to obtain those guarantees together with congestion
control; this project implements the reliability mechanisms explicitly.

## Results

The required experiment sends a random 1 MiB file with a 50 ms delay in each
direction and the default 250 ms timeout. Each configuration is measured three
times with GNU `/usr/bin/time`; every output is compared byte for byte with the
input. Sender time includes registration and FIN acknowledgment, and excludes
the receiver's two-second linger.

Measurements below were made locally under Ubuntu/WSL2 with GCC 13.3.0 using the
bundled compatibility relay. Twelve independent trials ran concurrently, each
with its own relay, port, files, and endpoints. These are measured local results;
repeat against the official relay in the course environments before submission.

| Window | Loss | Corrupt | Dup | Mean seconds | Throughput (KiB/s) |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0 | 0 | 0 | 104.190 | 9.828 |
| 16 | 0 | 0 | 0 | 6.723 | 152.305 |
| 1 | 0.05 | 0 | 0 | 133.047 | 7.697 |
| 16 | 0.05 | 0 | 0 | 21.320 | 48.030 |

Throughput is `1024 / mean_seconds`. Stop-and-wait sends 1024 DATA packets plus
one FIN, so its observed round trip is `104.190 / 1025 = 101.649 ms`. The
roughly 1.649 ms beyond the relay's configured 100 ms comes from process
scheduling, Python relay dispatch, socket handling, packet processing, and timer
granularity; the small registration/startup cost is also included in elapsed
time.

The clean window-16 transfer is about `104.190 / 6.723 = 15.50×` faster, close
to the ideal factor of 16. It overlaps the DATA round trips, but still requires
a separate FIN round trip after the final DATA acknowledgment. Startup,
scheduling, and the last window also keep the speedup below the ideal.

Five percent loss increases the window-1 time by about 1.28× and the window-16
time by about 3.17×. Loss applies to both DATA and ACKs. With window 1, a timeout
repeats one packet. With window 16, losing a DATA packet makes the receiver
discard later packets beyond the gap, and a timeout resends the entire
unacknowledged window. Cumulative ACKs can hide an individual lost ACK, but
they cannot repair a missing DATA packet.

Raw trials, environment metadata, and detailed results are retained in
[docs/benchmark](docs/benchmark). To reproduce:

```bash
make benchmark
# Or run all twelve independent trials concurrently:
python3 scripts/benchmark.py --jobs 12
# Official relay:
python3 scripts/benchmark.py --relay /path/to/cs425_relay.py
```

The benchmark saves its random input, raw CSV, means CSV, platform and binary
hashes, commands, per-run logs, and output files under `build/benchmark`.
The default uses four concurrent workers; `--jobs 1` runs sequentially.

## Tests and coverage

```bash
make check       # 22 Unity tests; no relay or real clock needed
make report      # enforce 100% line AND branch coverage of src/lab.c
make leak        # no-argument application path under ASan/UBSan
make leak-test   # complete Unity suite under ASan/UBSan with leak detection
make integration # 12 real transfers, CLI cases, and scripted I/O edge tests
python3 scripts/io-edge-tests.py --slow # additionally verify real 30 s idle limit
```

`make report` requires `gcovr`. If it is unavailable, install it in a virtual
environment and keep that environment active while generating reports:

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install gcovr
make report
```

The 22 tests cover every public function in `src/lab.h`, including exact PDF
wire fixtures, odd-length checksums, malformed datagrams, receiver gaps and
duplicates, cumulative ACKs, full windows, ring reuse, retransmission limits,
empty files, exact 1024-byte multiples, and timer boundaries. Three fixed-seed
in-memory transfers independently introduce 20% loss, corruption, and
duplication in each direction and verify the complete delivered bytes.

Local protocol coverage is **178/178 executable lines and 152/152 branches
(100%)**, with no exclusions inside the protocol layer. The HTML report is
`build/report/html/coverage_report.html`. Socket/file/CLI code is exercised
separately by the integration suite, including HELLO retry exhaustion, relay
refusal, corrupted ACKs, whole-window retries, lost final ACKs, fixed FIN linger,
and runtime/argument errors. AddressSanitizer, UndefinedBehaviorSanitizer, and
leak checks pass.

## Submission report and CI

```bash
bash scripts/create-submission-report.sh    # requires pandoc; writes MD and DOCX
bash scripts/create-submission-report.sh -m # Markdown only
```

The report generator checks the build, unit tests, coverage, and sanitizers
before recording their output and project sources. The generated files are
[submission-report.md](submission-report.md) and
[submission-report.docx](submission-report.docx).

GitHub Actions is configured to build and test GCC and Clang, enforce coverage
with GCC, run sanitizers, and run local integration tests on pushes and pull
requests. After pushing, confirm the **Build and test** workflow is green.
Then run **Create submission report** manually from the Actions tab, download
its `submission-report` artifact, and submit its DOCX to Canvas. Check the Canvas
submission afterward.

## Development notes and known limitations

The main implementation challenge is preserving protocol behavior while keeping
it independent of I/O: the same state machines drive both real UDP transfers
and deterministic tests. Another important edge case is the final ACK: receiver
linger permits retransmitted FINs to succeed after that ACK is lost.

No functional failures remain in the local test suites. The official Canvas
relay, Onyx, Codespaces, and remote GitHub Actions have not been verified from
this workspace. A timeout longer than the receiver's fixed two-second linger
cannot recover a lost final ACK; the default 250 ms timeout fits the specified
linger. The protocol has no authentication, congestion control, or sequence
wrap handling; the assignment bounds transfer size so sequence numbers do not
wrap.


---

## Build Output

Command: `make all`

```text
make BUILD=release _build
make[1]: Entering directory '/mnt/c/Users/davis/Downloads/cs425-p2'
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2 -DNDEBUG -fstack-protector-strong -MMD -MP -c src/lab.c -o build/release/src/lab.o
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2 -DNDEBUG -fstack-protector-strong -MMD -MP -c src/main.c -o build/release/src/main.o
cc build/release/src/lab.o build/release/src/main.o    -o build/release/myapp
make[1]: Leaving directory '/mnt/c/Users/davis/Downloads/cs425-p2'
make BUILD=debug _build
make[1]: Entering directory '/mnt/c/Users/davis/Downloads/cs425-p2'
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -g3 -O0 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie -MMD -MP -c src/lab.c -o build/debug/src/lab.o
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -g3 -O0 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie -MMD -MP -c src/main.c -o build/debug/src/main.o
cc build/debug/src/lab.o build/debug/src/main.o  -fsanitize=address,undefined -no-pie  -o build/debug/myapp_d
make[1]: Leaving directory '/mnt/c/Users/davis/Downloads/cs425-p2'
make BUILD=test _build
make[1]: Entering directory '/mnt/c/Users/davis/Downloads/cs425-p2'
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -g -O0 --coverage -MMD -MP -c src/lab.c -o build/tests/src/lab.o
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -g -O0 --coverage -MMD -MP -c tests/harness/unity.c -o build/tests/tests/harness/unity.o
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -g -O0 --coverage -MMD -MP -c tests/lab-test.c -o build/tests/tests/lab-test.o
cc build/tests/src/lab.o build/tests/tests/harness/unity.o build/tests/tests/lab-test.o  --coverage  -o build/tests/myapp_t
make[1]: Leaving directory '/mnt/c/Users/davis/Downloads/cs425-p2'
make BUILD=debug-test _build
make[1]: Entering directory '/mnt/c/Users/davis/Downloads/cs425-p2'
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -g3 -O0 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie -MMD -MP -c src/lab.c -o build/debug-test/src/lab.o
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -g3 -O0 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie -MMD -MP -c tests/harness/unity.c -o build/debug-test/tests/harness/unity.o
cc -D_POSIX_C_SOURCE=200809L -Isrc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -g3 -O0 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie -MMD -MP -c tests/lab-test.c -o build/debug-test/tests/lab-test.o
cc build/debug-test/src/lab.o build/debug-test/tests/harness/unity.o build/debug-test/tests/lab-test.o  -fsanitize=address,undefined -no-pie  -o build/debug-test/myapp_td
make[1]: Leaving directory '/mnt/c/Users/davis/Downloads/cs425-p2'

```

---

## Unit Test Results

Command: `make check`

```text
make BUILD=test _build
make[1]: Entering directory '/mnt/c/Users/davis/Downloads/cs425-p2'
make[1]: Nothing to be done for '_build'.
make[1]: Leaving directory '/mnt/c/Users/davis/Downloads/cs425-p2'
./build/tests/myapp_t
tests/lab-test.c:768:test_checksum_rfc1071_and_odd_lengths:PASS
tests/lab-test.c:769:test_exact_wire_fixtures_and_network_order:PASS
tests/lab-test.c:770:test_packet_roundtrips_boundary_payloads:PASS
tests/lab-test.c:771:test_packet_encoder_rejects_invalid_arguments:PASS
tests/lab-test.c:772:test_packet_decoder_rejects_each_malformed_wire_condition:PASS
tests/lab-test.c:773:test_sender_initialization_limits:PASS
tests/lab-test.c:774:test_sender_window_slides_with_cumulative_ack:PASS
tests/lab-test.c:775:test_sender_empty_file_and_exact_payload_multiple:PASS
tests/lab-test.c:776:test_sender_ignores_duplicate_future_and_non_ack_packets:PASS
tests/lab-test.c:777:test_sender_timer_retransmits_entire_unacknowledged_window:PASS
tests/lab-test.c:778:test_sender_fails_after_ten_consecutive_timeouts_and_resets_on_progress:PASS
tests/lab-test.c:779:test_sender_fin_retransmission_and_ring_buffer_reuse:PASS
tests/lab-test.c:780:test_receiver_delivers_in_order_and_repeats_cumulative_ack:PASS
tests/lab-test.c:781:test_receiver_fin_must_be_in_order_and_linger_is_fixed:PASS
tests/lab-test.c:782:test_receiver_empty_file_and_inactivity_boundary:PASS
tests/lab-test.c:783:test_receiver_valid_activity_resets_idle_timer:PASS
tests/lab-test.c:784:test_receiver_rejects_invalid_semantic_packets:PASS
tests/lab-test.c:785:test_receiver_refuses_file_above_limit:PASS
tests/lab-test.c:786:test_public_state_machine_null_arguments_are_safe:PASS
tests/lab-test.c:787:test_virtual_clock_boundaries_do_not_wrap:PASS
tests/lab-test.c:788:test_in_memory_channel_clean_empty_and_exact_multiple_files:PASS
tests/lab-test.c:789:test_in_memory_channel_fixed_seed_bidirectional_damage:PASS

-----------------------
22 Tests 0 Failures 0 Ignored
OK

```

---

## Coverage Report

Command: `make report`

```text
make BUILD=test _build
make[1]: Entering directory '/mnt/c/Users/davis/Downloads/cs425-p2'
make[1]: Nothing to be done for '_build'.
make[1]: Leaving directory '/mnt/c/Users/davis/Downloads/cs425-p2'
find build/tests -name '*.gcda' -delete
./build/tests/myapp_t
tests/lab-test.c:768:test_checksum_rfc1071_and_odd_lengths:PASS
tests/lab-test.c:769:test_exact_wire_fixtures_and_network_order:PASS
tests/lab-test.c:770:test_packet_roundtrips_boundary_payloads:PASS
tests/lab-test.c:771:test_packet_encoder_rejects_invalid_arguments:PASS
tests/lab-test.c:772:test_packet_decoder_rejects_each_malformed_wire_condition:PASS
tests/lab-test.c:773:test_sender_initialization_limits:PASS
tests/lab-test.c:774:test_sender_window_slides_with_cumulative_ack:PASS
tests/lab-test.c:775:test_sender_empty_file_and_exact_payload_multiple:PASS
tests/lab-test.c:776:test_sender_ignores_duplicate_future_and_non_ack_packets:PASS
tests/lab-test.c:777:test_sender_timer_retransmits_entire_unacknowledged_window:PASS
tests/lab-test.c:778:test_sender_fails_after_ten_consecutive_timeouts_and_resets_on_progress:PASS
tests/lab-test.c:779:test_sender_fin_retransmission_and_ring_buffer_reuse:PASS
tests/lab-test.c:780:test_receiver_delivers_in_order_and_repeats_cumulative_ack:PASS
tests/lab-test.c:781:test_receiver_fin_must_be_in_order_and_linger_is_fixed:PASS
tests/lab-test.c:782:test_receiver_empty_file_and_inactivity_boundary:PASS
tests/lab-test.c:783:test_receiver_valid_activity_resets_idle_timer:PASS
tests/lab-test.c:784:test_receiver_rejects_invalid_semantic_packets:PASS
tests/lab-test.c:785:test_receiver_refuses_file_above_limit:PASS
tests/lab-test.c:786:test_public_state_machine_null_arguments_are_safe:PASS
tests/lab-test.c:787:test_virtual_clock_boundaries_do_not_wrap:PASS
tests/lab-test.c:788:test_in_memory_channel_clean_empty_and_exact_multiple_files:PASS
tests/lab-test.c:789:test_in_memory_channel_fixed_seed_bidirectional_damage:PASS

-----------------------
22 Tests 0 Failures 0 Ignored
OK
gcovr --root . --filter 'src/lab\.c$' --object-directory build/tests --fail-under-line 100 --fail-under-branch 100 --print-summary --txt build/report/txt/coverage_report.txt --html-details build/report/html/coverage_report.html build/tests
(INFO) Reading coverage data...
(INFO) Writing coverage report...
lines: 100.0% (178 out of 178)
functions: 100.0% (13 out of 13)
branches: 100.0% (152 out of 152)
------------------------------------------------------------------------------
                           GCC Code Coverage Report
Directory: .
------------------------------------------------------------------------------
File                                       Lines    Exec  Cover   Missing
------------------------------------------------------------------------------
src/lab.c                                    178     178   100%
------------------------------------------------------------------------------
TOTAL                                        178     178   100%
------------------------------------------------------------------------------

```

---

## Application Address/Undefined Sanitizers

Command: `make leak`

```text
make BUILD=debug _build
make[1]: Entering directory '/mnt/c/Users/davis/Downloads/cs425-p2'
make[1]: Nothing to be done for '_build'.
make[1]: Leaving directory '/mnt/c/Users/davis/Downloads/cs425-p2'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/debug/myapp_d
Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]
                  [-c corrupt] [-d dup] [-p port] <relay> <file>
       myapp recv -s <session> [-p port] <relay> <file>
Defaults: window=8, timeout-ms=250, loss=corrupt=dup=0, port=4250
Session: 1-32 lowercase letters, digits, or hyphens. Window: 1-64.
Probabilities: 0 through 0.5. File size: at most 16 MiB.

```

---

## Unit Test Address/Undefined Sanitizers

Command: `make leak-test`

```text
make BUILD=debug-test _build
make[1]: Entering directory '/mnt/c/Users/davis/Downloads/cs425-p2'
make[1]: Nothing to be done for '_build'.
make[1]: Leaving directory '/mnt/c/Users/davis/Downloads/cs425-p2'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/debug-test/myapp_td
tests/lab-test.c:768:test_checksum_rfc1071_and_odd_lengths:PASS
tests/lab-test.c:769:test_exact_wire_fixtures_and_network_order:PASS
tests/lab-test.c:770:test_packet_roundtrips_boundary_payloads:PASS
tests/lab-test.c:771:test_packet_encoder_rejects_invalid_arguments:PASS
tests/lab-test.c:772:test_packet_decoder_rejects_each_malformed_wire_condition:PASS
tests/lab-test.c:773:test_sender_initialization_limits:PASS
tests/lab-test.c:774:test_sender_window_slides_with_cumulative_ack:PASS
tests/lab-test.c:775:test_sender_empty_file_and_exact_payload_multiple:PASS
tests/lab-test.c:776:test_sender_ignores_duplicate_future_and_non_ack_packets:PASS
tests/lab-test.c:777:test_sender_timer_retransmits_entire_unacknowledged_window:PASS
tests/lab-test.c:778:test_sender_fails_after_ten_consecutive_timeouts_and_resets_on_progress:PASS
tests/lab-test.c:779:test_sender_fin_retransmission_and_ring_buffer_reuse:PASS
tests/lab-test.c:780:test_receiver_delivers_in_order_and_repeats_cumulative_ack:PASS
tests/lab-test.c:781:test_receiver_fin_must_be_in_order_and_linger_is_fixed:PASS
tests/lab-test.c:782:test_receiver_empty_file_and_inactivity_boundary:PASS
tests/lab-test.c:783:test_receiver_valid_activity_resets_idle_timer:PASS
tests/lab-test.c:784:test_receiver_rejects_invalid_semantic_packets:PASS
tests/lab-test.c:785:test_receiver_refuses_file_above_limit:PASS
tests/lab-test.c:786:test_public_state_machine_null_arguments_are_safe:PASS
tests/lab-test.c:787:test_virtual_clock_boundaries_do_not_wrap:PASS
tests/lab-test.c:788:test_in_memory_channel_clean_empty_and_exact_multiple_files:PASS
tests/lab-test.c:789:test_in_memory_channel_fixed_seed_bidirectional_damage:PASS

-----------------------
22 Tests 0 Failures 0 Ignored
OK

```

---

## UDP Transfer and I/O Edge Tests

Command: `make integration`

```text
make BUILD=release _build
make[1]: Entering directory '/mnt/c/Users/davis/Downloads/cs425-p2'
make[1]: Nothing to be done for '_build'.
make[1]: Leaving directory '/mnt/c/Users/davis/Downloads/cs425-p2'
python3 scripts/integration-test.py --app ./build/release/myapp
PASS clean: 5120 bytes, sender 0.040s
PASS empty: 0 bytes, sender 0.020s
PASS one-byte: 1 bytes, sender 0.040s
PASS 1023-bytes: 1023 bytes, sender 0.030s
PASS 1024-bytes: 1024 bytes, sender 0.030s
PASS 1025-bytes: 1025 bytes, sender 0.030s
PASS loss: 10240 bytes, sender 1.330s
PASS corrupt: 10240 bytes, sender 0.820s
PASS duplicate: 10240 bytes, sender 0.060s
PASS mixed: 10240 bytes, sender 1.070s
PASS window-one: 3072 bytes, sender 0.310s
PASS window-max: 25600 bytes, sender 0.820s
PASS CLI: 11 usage/validation cases
All 12 transfers matched byte for byte. Logs: build/integration
python3 scripts/io-edge-tests.py --app ./build/release/myapp
PASS lost_registration_reply
PASS registration_attempt_limit
PASS registration_error
PASS lost_ack_retransmits_window
PASS ten_sender_timeouts
PASS malformed_receiver_datagrams
PASS lost_final_ack_fixed_linger
PASS file_and_hostname_errors
PASS invalid_cli
All 9 I/O edge tests passed.

```

---

## Performance Experiment

# Measured P2 transfer results

Input: 1,048,576 random bytes from `os.urandom`; 3 trials per configuration. Relay delay: 50 ms each direction. Timeout: 250 ms. Parallel runs: 12.

Every output file was compared byte for byte with the input. Sender elapsed time excludes the receiver's post-FIN linger. Raw trials are in `raw.csv`; commands and sender/receiver/relay logs are retained per run.

Measurements use the bundled **local compatibility relay**, which is separate from the instructor's Canvas relay. Re-run with `--relay /path/to/cs425_relay.py` to measure against the official script.

| Window | Loss | Mean sender seconds | Std. dev. | KiB/s |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0% | 104.190 | 0.010 | 9.828 |
| 16 | 0% | 6.723 | 0.006 | 152.305 |
| 1 | 5% | 133.047 | 1.472 | 7.697 |
| 16 | 5% | 21.320 | 1.777 | 48.030 |

Platform: `Linux-6.6.87.2-microsoft-standard-WSL2-x86_64-with-glibc2.39`.
Input SHA-256: `720f23e280d3a9c095d9b27268027c1b088cc62afa60883edc44bc5fde91fd1c`.

GNU `/usr/bin/time` provides elapsed/user/system time and peak RSS when installed. The script otherwise records sender wall time using Python's monotonic performance clock.


---

## Project Sources

### scripts/benchmark.py

```python
#!/usr/bin/env python3
"""Measure the four P2 configurations, three real 1 MiB transfers each."""

import argparse
import concurrent.futures
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

from transfer_support import transfer


CONFIGURATIONS = [
    ("w1-clean", 1, 0.0),
    ("w16-clean", 16, 0.0),
    ("w1-loss5", 1, 0.05),
    ("w16-loss5", 16, 0.05),
]


def write_csv(path, fields, rows):
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", default="build/release/myapp")
    parser.add_argument("--output", default="build/benchmark")
    parser.add_argument("--jobs", type=int, default=4, help="independent concurrent relays (default: 4)")
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--size", type=int, default=1024 * 1024, help="input bytes (default: 1 MiB)")
    parser.add_argument("--relay", help="relay script path; default is the bundled compatibility relay")
    parser.add_argument("--deadline", type=float, default=600, help="per-sender time limit in seconds")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 12 or args.trials < 1 or args.size < 0 or args.deadline <= 0:
        parser.error("jobs must be 1..12, trials positive, size nonnegative, and deadline positive")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    app = Path(args.app).resolve()
    if not app.is_file():
        parser.error(f"build the application first: {app}")
    payload = os.urandom(args.size)
    (output / "input.bin").write_bytes(payload)
    relay = Path(args.relay).resolve() if args.relay else Path(__file__).with_name("cs425_relay.py").resolve()
    metadata = {
        "measured_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": platform.platform(), "machine": platform.machine(),
        "python": platform.python_version(), "jobs": args.jobs, "trials": args.trials,
        "bytes": args.size, "input_source": "os.urandom (operating-system random bytes)",
        "input_sha256": hashlib.sha256(payload).hexdigest(),
        "app": str(app), "app_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
        "relay": str(relay), "relay_sha256": hashlib.sha256(relay.read_bytes()).hexdigest(),
        "bundled_compatibility_relay": not bool(args.relay),
        "one_way_delay_ms": 50, "timeout_ms": 250,
    }
    (output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    fields = ["configuration", "trial", "window", "loss", "corrupt", "duplicate", "seed",
              "bytes", "elapsed_seconds", "wall_seconds", "user_seconds", "system_seconds",
              "max_rss_kib", "timer", "verified", "logs"]
    rows = []

    def run(configuration, window, loss, trial):
        seed = 7 + trial - 1
        result = transfer(app, output / f"{configuration}-run{trial}", payload,
                          session=f"bench-{configuration}-{trial}", window=window,
                          loss=loss, delay=50, seed=seed, deadline=args.deadline,
                          relay_script=relay)
        return {"configuration": configuration, "trial": trial, "window": window,
                "loss": loss, "corrupt": 0.0, "duplicate": 0.0, "seed": seed, **result}

    # Interleave configurations so long stop-and-wait trials do not monopolize
    # every worker. Each run has its own relay, UDP port, files, and processes.
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(run, configuration, window, loss, trial)
                       for trial in range(1, args.trials + 1)
                       for configuration, window, loss in CONFIGURATIONS]
            for future in concurrent.futures.as_completed(futures):
                row = future.result()
                rows.append(row)
                rows.sort(key=lambda item: (item["configuration"], item["trial"]))
                write_csv(output / "raw.csv", fields, rows)
                print(f"PASS {row['configuration']} trial {row['trial']}: "
                      f"{row['elapsed_seconds']:.3f}s, {row['bytes']} bytes verified", flush=True)
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Benchmark failed: {error}\nCompleted measurements and logs: {output}", file=sys.stderr)
        return 1

    summaries = []
    for configuration, window, loss in CONFIGURATIONS:
        samples = [row["elapsed_seconds"] for row in rows if row["configuration"] == configuration]
        mean = statistics.mean(samples)
        summaries.append({"configuration": configuration, "window": window, "loss": loss,
                          "trials": len(samples), "mean_seconds": mean,
                          "stdev_seconds": statistics.stdev(samples) if len(samples) > 1 else 0.0,
                          "min_seconds": min(samples), "max_seconds": max(samples),
                          "mean_kib_per_second": args.size / 1024 / mean if mean else 0.0})
    write_csv(output / "means.csv", list(summaries[0]), summaries)
    report = ["# Measured P2 transfer results", "",
              f"Input: {args.size:,} random bytes from `os.urandom`; {args.trials} trials per configuration. "
              f"Relay delay: 50 ms each direction. Timeout: 250 ms. Parallel runs: {args.jobs}.", "",
              "Every output file was compared byte for byte with the input. Sender elapsed time "
              "excludes the receiver's post-FIN linger. Raw trials are in `raw.csv`; commands and "
              "sender/receiver/relay logs are retained per run.", ""]
    if not args.relay:
        report += ["Measurements use the bundled **local compatibility relay**, which is separate "
                   "from the instructor's Canvas relay. Re-run with `--relay /path/to/cs425_relay.py` "
                   "to measure against the official script.", ""]
    report += ["| Window | Loss | Mean sender seconds | Std. dev. | KiB/s |",
               "| ---: | ---: | ---: | ---: | ---: |"]
    for summary in summaries:
        report.append(f"| {summary['window']} | {summary['loss']:.0%} | "
                      f"{summary['mean_seconds']:.3f} | {summary['stdev_seconds']:.3f} | "
                      f"{summary['mean_kib_per_second']:.3f} |")
    report += ["", f"Platform: `{metadata['platform']}`.",
               f"Input SHA-256: `{metadata['input_sha256']}`.", "",
               "GNU `/usr/bin/time` provides elapsed/user/system time and peak RSS when installed. "
               "The script otherwise records sender wall time using Python's monotonic performance clock.", ""]
    (output / "results.md").write_text("\n".join(report))
    print("\n" + "\n".join(report))
    print(f"Artifacts: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

```

### scripts/cs425_relay.py

```python
#!/usr/bin/env python3
"""Local compatibility relay for CS425 P2; not the instructor's Canvas relay.

Registration traffic is reliable. Subsequent UDP datagrams are independently
subject to loss, bit corruption, duplication, and fixed delay in each direction.
"""

from __future__ import annotations

import argparse
import collections
import heapq
import random
import re
import select
import signal
import socket
import time
from dataclasses import dataclass, field


@dataclass
class Session:
    receiver: tuple
    sender: tuple | None = None
    rates: tuple = (0.0, 0.0, 0.0)
    active: float = field(default_factory=time.monotonic)
    rng: dict = field(default_factory=dict)
    counts: dict = field(default_factory=lambda: collections.defaultdict(collections.Counter))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=4250, help="loopback UDP port (default: 4250)")
    parser.add_argument("--delay", type=float, default=0.0, help="one-way delay in milliseconds")
    parser.add_argument("--seed", type=int, default=7, help="repeatable fault RNG seed")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 0 <= args.delay < float("inf"):
        parser.error("port must be 1..65535 and delay must be finite and nonnegative")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", args.port))
    sessions = {}
    queue = []
    serial = 0
    running = True
    totals = collections.Counter()

    def stop(_signum, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    def reply(address, message):
        sock.sendto(message.encode("ascii"), address)
        if args.verbose:
            print(f"registration {address}: {message}", flush=True)

    def register(payload, address, now):
        try:
            parts = payload.decode("ascii").split()
            if len(parts) not in (3, 6) or parts[0] != "HELLO":
                raise ValueError("malformed HELLO")
            name, role = parts[1:3]
            if re.fullmatch(r"[a-z0-9-]{1,32}", name) is None:
                raise ValueError("invalid session")
            if role == "recv" and len(parts) == 3:
                existing = sessions.get(name)
                if existing is None or existing.receiver != address:
                    existing = Session(receiver=address)
                    # Distinct streams make each direction's fault decisions independent.
                    existing.rng = {
                        "data": random.Random(f"{args.seed}:{name}:data"),
                        "ack": random.Random(f"{args.seed}:{name}:ack"),
                    }
                    sessions[name] = existing
                existing.active = now
            elif role == "send" and len(parts) == 6:
                rates = tuple(float(value) for value in parts[3:])
                if any(not 0.0 <= value <= 0.5 for value in rates):
                    raise ValueError("rates must be 0..0.5")
                existing = sessions.get(name)
                if existing is None:
                    raise ValueError("no receiver")
                if address == existing.receiver:
                    raise ValueError("sender and receiver need distinct sockets")
                if existing.sender not in (None, address):
                    raise ValueError("sender already registered")
                existing.sender = address
                existing.rates = rates
                existing.active = now
            else:
                raise ValueError("invalid role or argument count")
        except (UnicodeError, ValueError) as error:
            reply(address, f"ERR {error}")
            return
        reply(address, "OK")

    print(f"Local compatibility relay listening on 127.0.0.1:{args.port}; "
          f"delay={args.delay:g} ms, seed={args.seed}", flush=True)
    try:
        while running:
            now = time.monotonic()
            while queue and queue[0][0] <= now:
                _, _, destination, payload = heapq.heappop(queue)
                sock.sendto(payload, destination)
                totals["forwarded"] += 1
            for name, session in list(sessions.items()):
                if now - session.active >= 30.0:
                    if args.verbose:
                        print(f"session {name} expired after 30 seconds idle", flush=True)
                    del sessions[name]
            timeout = min(0.5, max(0.0, queue[0][0] - now)) if queue else 0.5
            if not select.select([sock], [], [], timeout)[0]:
                continue
            payload, address = sock.recvfrom(65535)
            now = time.monotonic()
            if payload.startswith(b"HELLO"):
                register(payload, address, now)
                continue
            matched = next(((name, session) for name, session in sessions.items()
                            if address in (session.sender, session.receiver)), None)
            if matched is None:
                totals["unknown"] += 1
                continue
            name, session = matched
            session.active = now
            direction = "data" if address == session.sender else "ack"
            destination = session.receiver if direction == "data" else session.sender
            if destination is None:
                continue
            counts = session.counts[direction]
            rng = session.rng[direction]
            loss, corrupt, duplicate = session.rates
            counts["received"] += 1
            totals["received"] += 1
            if rng.random() < loss:
                counts["lost"] += 1
                totals["lost"] += 1
                if args.verbose:
                    print(f"{name} {direction}: lost {len(payload)} bytes", flush=True)
                continue
            copies = 1
            if payload and rng.random() < corrupt:
                damaged = bytearray(payload)
                damaged[rng.randrange(len(damaged))] ^= 1 << rng.randrange(8)
                payload = bytes(damaged)
                counts["corrupted"] += 1
                totals["corrupted"] += 1
            elif rng.random() < duplicate:
                copies = 2
            if copies == 2:
                counts["duplicated"] += 1
                totals["duplicated"] += 1
            for _ in range(copies):
                serial += 1
                heapq.heappush(queue, (now + args.delay / 1000.0, serial, destination, payload))
            if args.verbose:
                print(f"{name} {direction}: queued {copies} x {len(payload)} bytes", flush=True)
    finally:
        sock.close()
        print("Relay summary: " + ", ".join(f"{key}={totals[key]}" for key in
              ("received", "forwarded", "lost", "corrupted", "duplicated", "unknown")), flush=True)
        for name, session in sessions.items():
            for direction, counts in session.counts.items():
                print(f"  {name} {direction}: {dict(counts)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

```

### scripts/integration-test.py

```python
#!/usr/bin/env python3
"""Run bounded local file-transfer tests using the bundled compatibility relay."""

import argparse
from pathlib import Path
import subprocess
import sys

from transfer_support import transfer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", default="build/release/myapp")
    parser.add_argument("--output", default="build/integration")
    parser.add_argument("--quick", action="store_true", help="only a clean binary transfer")
    args = parser.parse_args()
    app = str(Path(args.app).resolve())
    cases = [("clean", bytes(range(256)) * 20, {})]
    if not args.quick:
        cases += [
            ("empty", b"", {}),
            ("one-byte", b"\x00", {}),
            ("1023-bytes", bytes(range(256)) * 3 + bytes(range(255)), {}),
            ("1024-bytes", bytes(range(256)) * 4, {}),
            ("1025-bytes", bytes(range(256)) * 4 + b"\xff", {}),
            ("loss", bytes(range(256)) * 40, {"loss": 0.2}),
            ("corrupt", bytes(range(256)) * 40, {"corrupt": 0.2}),
            ("duplicate", bytes(range(256)) * 40, {"duplicate": 0.5}),
            ("mixed", bytes(range(256)) * 40,
             {"loss": 0.1, "corrupt": 0.1, "duplicate": 0.2}),
            ("window-one", bytes(range(256)) * 12, {"window": 1, "loss": 0.1}),
            ("window-max", bytes(range(256)) * 100, {"window": 64, "loss": 0.1}),
        ]
    try:
        for name, payload, options in cases:
            result = transfer(app, Path(args.output) / name, payload,
                              session=f"test-{name}", deadline=60, **options)
            print(f"PASS {name}: {len(payload)} bytes, sender {result['elapsed_seconds']:.3f}s", flush=True)
        if not args.quick:
            invalid = [[], ["send"], ["recv"], ["other"],
                       ["send", "-s", "BAD", "localhost", "input"],
                       ["send", "-s", "ok", "-w", "0", "localhost", "input"],
                       ["send", "-s", "ok", "-w", "65", "localhost", "input"],
                       ["send", "-s", "ok", "-l", "0.6", "localhost", "input"],
                       ["send", "-s", "ok", "-T", "0", "localhost", "input"],
                       ["recv", "-s", "ok", "-p", "0", "localhost", "output"],
                       ["recv", "-s", "ok", "-w", "8", "localhost", "output"]]
            for command in invalid:
                expected = 0 if not command else 1
                result = subprocess.run([app] + command, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=5, check=False)
                if result.returncode != expected:
                    raise RuntimeError(f"CLI {command!r}: exit {result.returncode}, wanted {expected}: "
                                       + result.stdout.decode(errors="replace"))
            print(f"PASS CLI: {len(invalid)} usage/validation cases", flush=True)
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"All {len(cases)} transfers matched byte for byte. Logs: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

```

### scripts/io-edge-tests.py

```python
#!/usr/bin/env python3
"""Exercise socket, registration, timeout and CLI edges with a scripted peer.

No external relay or packages are needed. The default suite takes about ten
seconds; --slow also checks the receiver's real 30-second inactivity timeout.
"""

import argparse
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


DATA, ACK, FIN = 0, 1, 2
HEADER_SIZE = 10
MAX_FILE_SIZE = 16 * 1024 * 1024


class TestFailure(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise TestFailure(message)


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack("!" + "H" * (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def packet(kind, sequence, payload=b"", reserved=0):
    header = struct.pack("!BBHIH", kind, reserved, 0, sequence, len(payload))
    wire = header + payload
    return struct.pack("!BBHIH", kind, reserved, checksum(wire), sequence,
                       len(payload)) + payload


def decode(wire):
    require(len(wire) >= HEADER_SIZE, f"short outgoing datagram: {wire!r}")
    kind, reserved, _, sequence, length = struct.unpack("!BBHIH", wire[:HEADER_SIZE])
    require(kind in (DATA, ACK, FIN), f"unknown outgoing type {kind}")
    require(reserved == 0 and length <= 1024, "invalid outgoing header")
    require(len(wire) == HEADER_SIZE + length, "outgoing length mismatch")
    require(kind == DATA or length == 0, "outgoing control packet has payload")
    require(checksum(wire) == 0, "outgoing checksum failed")
    return kind, sequence, wire[HEADER_SIZE:]


class ScriptedRelay:
    def __init__(self, app, directory, mode="send", payload=b"", extra=(),
                 host="127.0.0.1"):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.process = None
        self.peer = None
        self.path = directory / ("input.bin" if mode == "send" else "output.bin")
        if mode == "send":
            self.path.write_bytes(payload)
        port = self.socket.getsockname()[1]
        command = [app, mode, "-s", "edge", "-p", str(port), *extra, host,
                   str(self.path)]
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.kill()
            self.process.communicate()
        self.socket.close()

    def receive(self, timeout=2):
        self.socket.settimeout(timeout)
        wire, peer = self.socket.recvfrom(65535)
        if self.peer is None:
            self.peer = peer
        require(peer == self.peer, "client changed its UDP socket")
        return wire

    def send(self, wire):
        require(self.peer is not None, "peer has not registered")
        self.socket.sendto(wire, self.peer)

    def register(self, mode="send"):
        expected = b"HELLO edge send 0 0 0" if mode == "send" else b"HELLO edge recv"
        require(self.receive() == expected, "HELLO differs from the exact protocol text")
        self.send(b"OK")

    def expect_packet(self, kind, sequence, timeout=2):
        wire = self.receive(timeout)
        decoded = decode(wire)
        require(decoded[:2] == (kind, sequence),
                f"wanted type/sequence {(kind, sequence)}, received {decoded[:2]}")
        return wire, decoded[2]

    def expect_silence(self, timeout=0.15):
        try:
            unexpected = self.receive(timeout)
        except socket.timeout:
            return
        raise TestFailure(f"invalid packet caused a response: {unexpected!r}")

    def finish(self, expected=0, timeout=3):
        out, err = self.process.communicate(timeout=timeout)
        require(self.process.returncode == expected,
                f"exit {self.process.returncode}, expected {expected}: "
                + (out + err).decode(errors="replace"))
        return out.decode(errors="replace"), err.decode(errors="replace")


def lost_registration_reply(app, directory):
    # A hostname also verifies getaddrinfo's localhost path.
    with ScriptedRelay(app, directory, host="localhost") as relay:
        first = relay.receive()
        started = time.monotonic()
        require(first == b"HELLO edge send 0 0 0", "HELLO has extra bytes/newline")
        require(relay.receive() == first, "registration retry changed HELLO")
        require(time.monotonic() - started >= 0.8, "registration retried before one second")
        relay.send(b"OK")
        relay.expect_packet(FIN, 0)
        relay.send(packet(ACK, 1))
        relay.finish()


def registration_attempt_limit(app, directory):
    with ScriptedRelay(app, directory) as relay:
        started = time.monotonic()
        for _ in range(5):
            require(relay.receive() == b"HELLO edge send 0 0 0", "bad retry HELLO")
        _, err = relay.finish(expected=2, timeout=2)
        require(time.monotonic() - started >= 4.8, "five one-second attempts ended early")
        require("five attempts" in err, "registration failure lacks its reason")
        relay.expect_silence(timeout=0.05)


def registration_error(app, directory):
    with ScriptedRelay(app, directory) as relay:
        relay.receive()
        relay.send(b"ERR deliberate test refusal")
        _, err = relay.finish(expected=2)
        require("deliberate test refusal" in err, "relay ERR reason was not printed")


def lost_ack_retransmits_window(app, directory):
    source = bytes(range(256)) * 9 + b"a final tail"
    with ScriptedRelay(app, directory, payload=source,
                       extra=("-w", "2", "-T", "50")) as relay:
        relay.register()
        original = [relay.expect_packet(DATA, sequence)[0] for sequence in (0, 1)]
        # Both a valid future ACK and a checksum-corrupt ACK must be ignored.
        relay.send(packet(ACK, 99))
        corrupt = bytearray(packet(ACK, 2))
        corrupt[2] ^= 1
        relay.send(bytes(corrupt))
        repeated = [relay.expect_packet(DATA, sequence)[0] for sequence in (0, 1)]
        require(original == repeated, "timeout did not resend the complete unchanged window")
        relay.send(packet(ACK, 2))
        _, tail = relay.expect_packet(DATA, 2)
        require(b"".join(decode(wire)[2] for wire in original) + tail == source,
                "sender payload differs from the binary input")
        relay.send(packet(ACK, 3))
        relay.expect_packet(FIN, 3)
        relay.send(packet(ACK, 4))
        relay.finish()


def ten_sender_timeouts(app, directory):
    with ScriptedRelay(app, directory, payload=b"x",
                       extra=("-w", "1", "-T", "10")) as relay:
        relay.register()
        _, err = relay.finish(expected=2, timeout=2)
        transmissions = []
        while True:
            try:
                transmissions.append(relay.receive(timeout=0.02))
            except socket.timeout:
                break
        require(len(transmissions) == 10,
                f"expected initial DATA plus nine retransmits, got {len(transmissions)}")
        require(all(decode(wire) == (DATA, 0, b"x") for wire in transmissions),
                "sender emitted unexpected data while ACKs were lost")
        require("10 consecutive timeouts" in err, "timeout failure lacks its reason")


def malformed_receiver_datagrams(app, directory):
    source = b"binary\x00\xff\x10payload"
    with ScriptedRelay(app, directory, mode="recv") as relay:
        relay.register(mode="recv")
        corrupt = bytearray(packet(DATA, 0, source))
        corrupt[-1] ^= 1
        malformed = [b"", b"short", b"\x00" * 9,
                     packet(DATA, 0, source, reserved=1),
                     packet(3, 0), packet(DATA, 0, source)[:-1],
                     packet(DATA, 0, source) + b"\x00",
                     packet(DATA, 0, b"x" * 1025),
                     packet(ACK, 0, b"x"), packet(FIN, 0, b"x"),
                     bytes(corrupt), packet(ACK, 0)]
        for wire in malformed:
            relay.send(wire)
        relay.expect_silence()
        relay.send(packet(DATA, 1, b"out of order"))
        relay.expect_packet(ACK, 0)
        relay.send(packet(DATA, 0, source))
        relay.expect_packet(ACK, 1)
        relay.send(packet(DATA, 0, b"duplicate must not replace payload"))
        relay.expect_packet(ACK, 1)
        relay.send(packet(FIN, 2))
        relay.expect_packet(ACK, 1)
        relay.send(packet(FIN, 1))
        relay.expect_packet(ACK, 2)
        relay.finish()
        require(relay.path.read_bytes() == source, "invalid/duplicate datagrams changed output")


def lost_final_ack_fixed_linger(app, directory):
    with ScriptedRelay(app, directory, mode="recv") as relay:
        relay.register(mode="recv")
        source = b"close the output before acknowledging FIN\x00\xff"
        relay.send(packet(DATA, 0, source))
        relay.expect_packet(ACK, 1)
        relay.send(packet(FIN, 1))
        relay.expect_packet(ACK, 2)  # Simulate losing this ACK at the sender.
        first_ack = time.monotonic()
        require(relay.path.read_bytes() == source, "output was not flushed/closed at FIN")
        time.sleep(1.6)
        relay.send(packet(FIN, 1))
        relay.expect_packet(ACK, 2)
        relay.finish(timeout=1.2)
        require(time.monotonic() - first_ack < 2.8,
                "duplicate FIN extended the fixed two-second linger")


def file_and_hostname_errors(app, directory):
    empty = directory / "empty.bin"
    empty.write_bytes(b"")
    oversized = directory / "too-large.bin"
    with oversized.open("wb") as output:
        output.truncate(MAX_FILE_SIZE + 1)
    cases = [
        ["send", "-s", "edge", "localhost", str(directory / "missing.bin")],
        ["send", "-s", "edge", "localhost", str(oversized)],
        ["send", "-s", "edge", "rdt-no-such-host.invalid", str(empty)],
    ]
    for command in cases:
        result = subprocess.run([app, *command], capture_output=True, timeout=10,
                                check=False)
        require(result.returncode == 2,
                f"file/hostname error exited {result.returncode}: {command!r}")
        require(result.stderr, "I/O failure did not explain itself")


def invalid_cli(app, directory):
    source = directory / "cli.bin"
    source.write_bytes(b"")
    base = ["send", "-s", "edge"]
    suffix = ["localhost", str(source)]
    commands = []
    for flag in ("-l", "-c", "-d"):
        for value in ("nan", "inf", "-inf", "1e309", "-0.1", "0.50001", ".", "oops"):
            commands.append(base + [flag, value] + suffix)
    for flag, value in (("-w", "0"), ("-w", "65"), ("-T", "0"),
                        ("-T", "-1"), ("-p", "0"), ("-p", "65536")):
        commands.append(base + [flag, value] + suffix)
    for session in ("", "BAD", "underscore_session", "x" * 33):
        commands.append(["send", "-s", session] + suffix)
    commands.append(["recv", "-s", "edge", "-l", "0.1"] + suffix)
    for command in commands:
        result = subprocess.run([app, *command], capture_output=True, timeout=3,
                                check=False)
        require(result.returncode == 1,
                f"invalid CLI exited {result.returncode}: {command!r}")


def receiver_inactivity(app, directory):
    with ScriptedRelay(app, directory, mode="recv") as relay:
        relay.register(mode="recv")
        started = time.monotonic()
        # Invalid traffic must not refresh the valid-packet inactivity timer.
        time.sleep(2)
        relay.send(b"not a valid packet")
        _, err = relay.finish(expected=2, timeout=29.5)
        require(time.monotonic() - started >= 29.5, "receiver timed out before 30 seconds")
        require("timed out" in err, "receiver inactivity failure lacks its reason")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", default="build/release/myapp")
    parser.add_argument("--slow", action="store_true",
                        help="also wait for the real 30-second receiver idle timeout")
    args = parser.parse_args()
    app = str(Path(args.app).resolve())
    cases = [lost_registration_reply, registration_attempt_limit, registration_error,
             lost_ack_retransmits_window, ten_sender_timeouts,
             malformed_receiver_datagrams, lost_final_ack_fixed_linger,
             file_and_hostname_errors, invalid_cli]
    if args.slow:
        cases.append(receiver_inactivity)
    try:
        with tempfile.TemporaryDirectory(prefix="rdt-io-") as temporary:
            root = Path(temporary)
            for test in cases:
                directory = root / test.__name__
                directory.mkdir()
                test(app, directory)
                print(f"PASS {test.__name__}", flush=True)
    except (TestFailure, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"All {len(cases)} I/O edge tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

```

### scripts/transfer_support.py

```python
"""Bounded subprocess helpers shared by the local tests and benchmark."""

import os
from pathlib import Path
import socket
import shlex
import signal
import subprocess
import sys
import time


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
        process.wait(timeout=2)


def transfer(app, directory, payload, *, session="p2-test", window=8,
             loss=0.0, corrupt=0.0, duplicate=0.0, delay=5, seed=7,
             retransmit_ms=250, deadline=90, relay_script=None):
    """Transfer bytes, verify exact output, and retain logs. Raise on failure."""
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    source, destination = directory / "input.bin", directory / "output.bin"
    source.write_bytes(payload)
    destination.unlink(missing_ok=True)
    app = str(Path(app).resolve())
    relay_script = str(Path(relay_script or Path(__file__).with_name("cs425_relay.py")).resolve())
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    relay = receiver = sender = None
    timing_file = directory / "sender-time.csv"
    timing_file.unlink(missing_ok=True)
    relay_command = [sys.executable, relay_script, "--port", str(port),
                     "--delay", str(delay), "--seed", str(seed)]
    receiver_command = [app, "recv", "-s", session, "-p", str(port),
                        "127.0.0.1", str(destination)]
    sender_command = [app, "send", "-s", session, "-p", str(port),
                      "-w", str(window), "-T", str(retransmit_ms),
                      "-l", str(loss), "-c", str(corrupt), "-d", str(duplicate),
                      "127.0.0.1", str(source)]
    (directory / "commands.txt").write_text(
        "\n".join(shlex.join(command) for command in
                  (relay_command, receiver_command, sender_command)) + "\n")
    try:
        with (directory / "relay.log").open("w") as relay_log, \
             (directory / "receiver.log").open("w") as receiver_log, \
             (directory / "sender.log").open("w") as sender_log:
            relay = subprocess.Popen(relay_command, stdout=relay_log, stderr=subprocess.STDOUT,
                                     start_new_session=(os.name == "posix"))
            ready_deadline = time.monotonic() + 5
            # A malformed HELLO must get ERR and creates no session. This also
            # works with the official script regardless of its startup logging.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.settimeout(0.1)
                while True:
                    if relay.poll() is not None or time.monotonic() >= ready_deadline:
                        raise RuntimeError(f"relay failed to start; see {directory / 'relay.log'}")
                    probe.sendto(b"HELLO", ("127.0.0.1", port))
                    try:
                        response, _ = probe.recvfrom(1024)
                        if response.startswith(b"ERR"):
                            break
                    except socket.timeout:
                        pass
            receiver = subprocess.Popen(receiver_command, stdout=receiver_log, stderr=subprocess.STDOUT,
                                        start_new_session=(os.name == "posix"))
            # The client creates its output only after the relay accepts HELLO.
            ready_deadline = time.monotonic() + 8
            while not destination.exists():
                if receiver.poll() is not None or time.monotonic() >= ready_deadline:
                    raise RuntimeError(f"receiver registration failed; see {directory / 'receiver.log'}")
                time.sleep(0.02)
            use_gnu_time = os.path.isfile("/usr/bin/time")
            measured_command = (["/usr/bin/time", "-f", "%e,%U,%S,%M", "-o", str(timing_file)]
                                + sender_command) if use_gnu_time else sender_command
            started = time.perf_counter()
            sender = subprocess.Popen(measured_command, stdout=sender_log, stderr=subprocess.STDOUT,
                                      start_new_session=(os.name == "posix"))
            sender_status = sender.wait(timeout=deadline)
            wall = time.perf_counter() - started
            if sender_status != 0:
                raise RuntimeError(f"sender exit {sender_status}; see {directory / 'sender.log'}")
            receiver_status = receiver.wait(timeout=min(30, deadline))
            if receiver_status != 0:
                raise RuntimeError(f"receiver exit {receiver_status}; see {directory / 'receiver.log'}")
            if not destination.is_file() or destination.read_bytes() != payload:
                raise RuntimeError(f"byte comparison failed in {directory}")
        elapsed, user, system, rss = wall, "", "", ""
        if timing_file.exists():
            elapsed, user, system, rss = timing_file.read_text().strip().split(",")
            elapsed, user, system, rss = float(elapsed), float(user), float(system), int(rss)
        return {"elapsed_seconds": elapsed, "wall_seconds": wall,
                "user_seconds": user, "system_seconds": system, "max_rss_kib": rss,
                "bytes": len(payload), "verified": True,
                "timer": "GNU time" if use_gnu_time else "perf_counter",
                "logs": str(directory)}
    finally:
        stop_process(sender)
        stop_process(receiver)
        stop_process(relay)

```

### src/lab.c

```c
#include "lab.h"

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

static bool valid_packet(const rdt_packet *packet)
{
    return packet != NULL && packet->type <= RDT_FIN &&
           packet->length <= RDT_PAYLOAD_SIZE &&
           (packet->type == RDT_DATA || packet->length == 0U);
}

static uint64_t deadline_after(uint64_t now_ms, uint64_t delay_ms)
{
    return delay_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + delay_ms;
}

uint16_t rdt_checksum(const uint8_t *bytes, size_t size)
{
    uint32_t sum = 0U;

    if (bytes == NULL && size != 0U) {
        return 0U;
    }
    while (size >= 2U) {
        sum += ((uint32_t)bytes[0] << 8U) | (uint32_t)bytes[1];
        /* Fold each word so arbitrarily large inputs cannot overflow. */
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16U);
        bytes += 2;
        size -= 2U;
    }
    if (size != 0U) {
        sum += (uint32_t)bytes[0] << 8U;
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16U);
    }
    return (uint16_t)(~sum & UINT32_C(0xffff));
}

size_t rdt_packet_encode(const rdt_packet *packet, uint8_t *bytes,
                         size_t capacity)
{
    size_t size;
    uint16_t checksum;
    uint16_t length;
    uint32_t sequence;

    if (!valid_packet(packet) || bytes == NULL) {
        return 0U;
    }
    size = RDT_HEADER_SIZE + (size_t)packet->length;
    if (capacity < size) {
        return 0U;
    }

    bytes[0] = packet->type;
    bytes[1] = 0U;
    bytes[2] = 0U;
    bytes[3] = 0U;
    sequence = htonl(packet->seq);
    length = htons(packet->length);
    memcpy(bytes + 4U, &sequence, sizeof(sequence));
    memcpy(bytes + 8U, &length, sizeof(length));
    if (packet->length != 0U) {
        memcpy(bytes + RDT_HEADER_SIZE, packet->payload, packet->length);
    }
    checksum = htons(rdt_checksum(bytes, size));
    memcpy(bytes + 2U, &checksum, sizeof(checksum));
    return size;
}

bool rdt_packet_decode(const uint8_t *bytes, size_t size, rdt_packet *packet)
{
    rdt_packet decoded;
    uint16_t length;
    uint32_t sequence;

    if (bytes == NULL || packet == NULL || size < RDT_HEADER_SIZE ||
        bytes[0] > RDT_FIN || bytes[1] != 0U) {
        return false;
    }
    memcpy(&length, bytes + 8U, sizeof(length));
    length = ntohs(length);
    if (length > RDT_PAYLOAD_SIZE ||
        size != RDT_HEADER_SIZE + (size_t)length ||
        (bytes[0] != RDT_DATA && length != 0U) ||
        rdt_checksum(bytes, size) != 0U) {
        return false;
    }

    memset(&decoded, 0, sizeof(decoded));
    decoded.type = bytes[0];
    memcpy(&sequence, bytes + 4U, sizeof(sequence));
    decoded.seq = ntohl(sequence);
    decoded.length = length;
    if (length != 0U) {
        memcpy(decoded.payload, bytes + RDT_HEADER_SIZE, length);
    }
    *packet = decoded;
    return true;
}

bool gbn_sender_init(gbn_sender *sender, const uint8_t *data, size_t size,
                     size_t window, uint64_t timeout_ms)
{
    if (sender == NULL) {
        return false;
    }
    memset(sender, 0, sizeof(*sender));
    sender->status = RDT_FAILED;
    if ((data == NULL && size != 0U) || size > RDT_MAX_FILE_SIZE ||
        window == 0U || window > RDT_MAX_WINDOW || timeout_ms == 0U) {
        return false;
    }
    sender->data = data;
    sender->size = size;
    sender->window = window;
    sender->timeout_ms = timeout_ms;
    sender->data_count = (uint32_t)((size + RDT_PAYLOAD_SIZE - 1U) /
                                   RDT_PAYLOAD_SIZE);
    sender->status = RDT_ACTIVE;
    return true;
}

void gbn_sender_send(gbn_sender *sender, uint64_t now_ms, gbn_batch *batch)
{
    if (batch == NULL) {
        return;
    }
    batch->count = 0U;
    if (sender == NULL || sender->status != RDT_ACTIVE) {
        return;
    }

    while (sender->next < sender->data_count &&
           (size_t)(sender->next - sender->base) < sender->window) {
        rdt_packet *packet = &sender->saved[sender->next % RDT_MAX_WINDOW];
        const size_t offset = (size_t)sender->next * RDT_PAYLOAD_SIZE;
        const size_t remaining = sender->size - offset;
        const size_t length = remaining < RDT_PAYLOAD_SIZE ?
                              remaining : RDT_PAYLOAD_SIZE;

        memset(packet, 0, sizeof(*packet));
        packet->type = RDT_DATA;
        packet->seq = sender->next;
        packet->length = (uint16_t)length;
        memcpy(packet->payload, sender->data + offset, length);
        if (sender->base == sender->next) {
            sender->deadline_ms = deadline_after(now_ms, sender->timeout_ms);
        }
        batch->packets[batch->count++] = *packet;
        ++sender->next;
    }

    /* FIN must wait for cumulative acknowledgement of every DATA packet. */
    if (sender->base == sender->data_count &&
        sender->next == sender->data_count) {
        rdt_packet *packet = &sender->saved[sender->next % RDT_MAX_WINDOW];

        memset(packet, 0, sizeof(*packet));
        packet->type = RDT_FIN;
        packet->seq = sender->data_count;
        batch->packets[batch->count++] = *packet;
        ++sender->next;
        sender->deadline_ms = deadline_after(now_ms, sender->timeout_ms);
    }
}

void gbn_sender_ack(gbn_sender *sender, const rdt_packet *packet,
                    uint64_t now_ms)
{
    if (sender == NULL || sender->status != RDT_ACTIVE ||
        !valid_packet(packet) || packet->type != RDT_ACK ||
        packet->seq <= sender->base || packet->seq > sender->next) {
        return;
    }
    sender->base = packet->seq;
    sender->consecutive_timeouts = 0U;
    sender->deadline_ms = sender->base < sender->next ?
        deadline_after(now_ms, sender->timeout_ms) : 0U;
    if (sender->base == sender->data_count + 1U) {
        sender->status = RDT_DONE;
    }
}

void gbn_sender_timeout(gbn_sender *sender, uint64_t now_ms, gbn_batch *batch)
{
    uint32_t sequence;

    if (batch == NULL) {
        return;
    }
    batch->count = 0U;
    if (sender == NULL || sender->status != RDT_ACTIVE ||
        sender->base == sender->next || now_ms < sender->deadline_ms) {
        return;
    }
    ++sender->consecutive_timeouts;
    if (sender->consecutive_timeouts >= RDT_MAX_TIMEOUTS) {
        sender->status = RDT_FAILED;
        return;
    }
    for (sequence = sender->base; sequence < sender->next; ++sequence) {
        batch->packets[batch->count++] =
            sender->saved[sequence % RDT_MAX_WINDOW];
    }
    sender->deadline_ms = deadline_after(now_ms, sender->timeout_ms);
}

void gbn_receiver_init(gbn_receiver *receiver, uint64_t now_ms)
{
    if (receiver != NULL) {
        memset(receiver, 0, sizeof(*receiver));
        receiver->last_activity_ms = now_ms;
        receiver->status = RDT_ACTIVE;
    }
}

rdt_status gbn_receiver_tick(gbn_receiver *receiver, uint64_t now_ms)
{
    if (receiver == NULL) {
        return RDT_FAILED;
    }
    if (receiver->status == RDT_ACTIVE) {
        if (receiver->finishing) {
            if (now_ms >= receiver->linger_until_ms) {
                receiver->status = RDT_DONE;
            }
        } else if (now_ms >= receiver->last_activity_ms &&
                   now_ms - receiver->last_activity_ms >=
                   RDT_RECEIVER_IDLE_MS) {
            receiver->status = RDT_FAILED;
        }
    }
    return receiver->status;
}

static void acknowledge(const gbn_receiver *receiver,
                        gbn_receive_action *action)
{
    action->ack = true;
    action->packet.type = RDT_ACK;
    action->packet.seq = receiver->expected;
}

void gbn_receiver_receive(gbn_receiver *receiver, const rdt_packet *packet,
                          uint64_t now_ms, gbn_receive_action *action)
{
    if (action == NULL) {
        return;
    }
    memset(action, 0, sizeof(*action));
    if (receiver == NULL || gbn_receiver_tick(receiver, now_ms) != RDT_ACTIVE ||
        !valid_packet(packet) || packet->type == RDT_ACK) {
        return;
    }
    if (receiver->finishing) {
        if (packet->type == RDT_FIN && packet->seq + 1U == receiver->expected) {
            acknowledge(receiver, action);
        }
        return;
    }

    receiver->last_activity_ms = now_ms;
    if (packet->type == RDT_DATA && packet->seq == receiver->expected) {
        if ((size_t)packet->length > RDT_MAX_FILE_SIZE - receiver->received_size) {
            receiver->status = RDT_FAILED;
            return;
        }
        action->deliver = packet->payload;
        action->size = packet->length;
        receiver->received_size += packet->length;
        ++receiver->expected;
    } else if (packet->type == RDT_FIN && packet->seq == receiver->expected) {
        ++receiver->expected;
        receiver->finishing = true;
        receiver->linger_until_ms = deadline_after(now_ms, RDT_LINGER_MS);
        action->finish = true;
    }
    acknowledge(receiver, action);
}

```

### src/lab.h

```c
#ifndef LAB_H
#define LAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RDT_HEADER_SIZE 10U
#define RDT_PAYLOAD_SIZE 1024U
#define RDT_MAX_PACKET_SIZE (RDT_HEADER_SIZE + RDT_PAYLOAD_SIZE)
#define RDT_MAX_FILE_SIZE (16U * 1024U * 1024U)
#define RDT_MAX_WINDOW 64U
#define RDT_MAX_TIMEOUTS 10U
#define RDT_RECEIVER_IDLE_MS UINT64_C(30000)
#define RDT_LINGER_MS UINT64_C(2000)

typedef enum {
    RDT_DATA = 0,
    RDT_ACK = 1,
    RDT_FIN = 2
} rdt_type;

typedef enum {
    RDT_ACTIVE = 0,
    RDT_DONE = 1,
    RDT_FAILED = 2
} rdt_status;

/* Host-order representation; never send this structure directly. */
typedef struct {
    uint8_t type;
    uint32_t seq;
    uint16_t length;
    uint8_t payload[RDT_PAYLOAD_SIZE];
} rdt_packet;

uint16_t rdt_checksum(const uint8_t *bytes, size_t size);
/* Encode returns the datagram size, or zero for invalid input/capacity. */
size_t rdt_packet_encode(const rdt_packet *packet, uint8_t *bytes,
                         size_t capacity);
/* Decode rejects malformed/corrupt datagrams without modifying packet. */
bool rdt_packet_decode(const uint8_t *bytes, size_t size, rdt_packet *packet);

typedef struct {
    rdt_packet packets[RDT_MAX_WINDOW];
    size_t count;
} gbn_batch;

typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t base;
    uint32_t next;
    uint32_t data_count;
    size_t window;
    uint64_t timeout_ms;
    uint64_t deadline_ms;
    unsigned consecutive_timeouts;
    rdt_status status;
    rdt_packet saved[RDT_MAX_WINDOW];
} gbn_sender;

/* Input data must remain valid and unchanged until the sender finishes. */
bool gbn_sender_init(gbn_sender *sender, const uint8_t *data, size_t size,
                     size_t window, uint64_t timeout_ms);
/* Each operation clears its output batch before adding packets to transmit. */
void gbn_sender_send(gbn_sender *sender, uint64_t now_ms, gbn_batch *batch);
void gbn_sender_ack(gbn_sender *sender, const rdt_packet *packet,
                    uint64_t now_ms);
void gbn_sender_timeout(gbn_sender *sender, uint64_t now_ms, gbn_batch *batch);

typedef struct {
    uint32_t expected;
    size_t received_size;
    uint64_t last_activity_ms;
    uint64_t linger_until_ms;
    bool finishing;
    rdt_status status;
} gbn_receiver;

typedef struct {
    bool ack;
    rdt_packet packet;
    /* A borrowed view of receive's packet payload, valid until it changes. */
    const uint8_t *deliver;
    size_t size;
    /* True once, when an in-order FIN requires the output file to close. */
    bool finish;
} gbn_receive_action;

void gbn_receiver_init(gbn_receiver *receiver, uint64_t now_ms);
void gbn_receiver_receive(gbn_receiver *receiver, const rdt_packet *packet,
                          uint64_t now_ms, gbn_receive_action *action);
rdt_status gbn_receiver_tick(gbn_receiver *receiver, uint64_t now_ms);

#endif

```

### src/main.c

```c
#define _POSIX_C_SOURCE 200809L
#include "lab.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Layer 3: all files, sockets and real clocks are confined to this adapter. */
struct options {
    bool sending;
    const char *session;
    const char *host;
    const char *file;
    unsigned window;
    unsigned timeout_ms;
    unsigned port;
    double loss, corrupt, duplicate;
};

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n"
        "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
        "       myapp recv -s <session> [-p port] <relay> <file>\n"
        "Defaults: window=8, timeout-ms=250, loss=corrupt=dup=0, port=4250\n"
        "Session: 1-32 lowercase letters, digits, or hyphens. Window: 1-64.\n"
        "Probabilities: 0 through 0.5. File size: at most 16 MiB.\n");
}

static bool integer_option(const char *text, unsigned maximum, unsigned *value)
{
    char *end;
    if (*text < '0' || *text > '9') return false;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0 || parsed > maximum)
        return false;
    *value = (unsigned)parsed;
    return true;
}

static bool probability_option(const char *text, double *value)
{
    char *end;
    if (!((*text >= '0' && *text <= '9') || *text == '.')) return false;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed) ||
        parsed < 0.0 || parsed > 0.5) return false;
    *value = parsed;
    return true;
}

static bool session_valid(const char *session)
{
    if (session == NULL) return false;
    size_t length = strlen(session);
    if (length == 0 || length > 32) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = session[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
    *options = (struct options){.window = 8, .timeout_ms = 250, .port = 4250};
    if (strcmp(argv[1], "send") == 0) options->sending = true;
    else if (strcmp(argv[1], "recv") != 0) return false;
    optind = 2;
    opterr = 0;
    int flag;
    while ((flag = getopt(argc, argv, "s:p:w:T:l:c:d:")) != -1) {
        if (!options->sending && flag != 's' && flag != 'p') return false;
        switch (flag) {
        case 's': options->session = optarg; break;
        case 'p':
            if (!integer_option(optarg, 65535, &options->port)) return false;
            break;
        case 'w':
            if (!integer_option(optarg, RDT_MAX_WINDOW, &options->window)) return false;
            break;
        case 'T':
            if (!integer_option(optarg, INT_MAX, &options->timeout_ms)) return false;
            break;
        case 'l':
            if (!probability_option(optarg, &options->loss)) return false;
            break;
        case 'c':
            if (!probability_option(optarg, &options->corrupt)) return false;
            break;
        case 'd':
            if (!probability_option(optarg, &options->duplicate)) return false;
            break;
        default: return false;
        }
    }
    if (argc - optind != 2 || !session_valid(options->session)) return false;
    options->host = argv[optind];
    options->file = argv[optind + 1];
    return options->host[0] != '\0' && options->file[0] != '\0';
}

static bool monotonic_ms(uint64_t *now)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) {
        perror("clock_gettime");
        return false;
    }
    *now = (uint64_t)stamp.tv_sec * UINT64_C(1000) +
           (uint64_t)stamp.tv_nsec / UINT64_C(1000000);
    return true;
}

/* Return 1 for readable, 0 when due, and -1 for an I/O failure. */
static int wait_until(int socket_fd, uint64_t deadline)
{
    for (;;) {
        uint64_t now;
        if (!monotonic_ms(&now)) return -1;
        if (now >= deadline) return 0;
        uint64_t remaining = deadline - now;
        int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd descriptor = {.fd = socket_fd, .events = POLLIN};
        int ready = poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { perror("poll"); return -1; }
        if (ready == 0) continue;
        if ((descriptor.revents & POLLIN) != 0) return 1;
        fprintf(stderr, "UDP socket failed (poll events %d).\n", descriptor.revents);
        return -1;
    }
}

static int connect_relay(const struct options *options)
{
    struct addrinfo hints = {0}, *addresses = NULL;
    char service[6];
    snprintf(service, sizeof service, "%u", options->port);
    /* The assignment's relay listens on IPv4 loopback. Resolve names as well. */
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int error = getaddrinfo(options->host, service, &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "Cannot resolve relay '%s': %s\n", options->host, gai_strerror(error));
        return -1;
    }
    int socket_fd = -1;
    for (const struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) continue;
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) break;
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket_fd < 0) perror("connect relay");
    return socket_fd;
}

static bool send_bytes(int socket_fd, const uint8_t *bytes, size_t length)
{
    ssize_t sent;
    do { sent = send(socket_fd, bytes, length, 0); } while (sent < 0 && errno == EINTR);
    if (sent < 0) { perror("send"); return false; }
    if ((size_t)sent != length) {
        fprintf(stderr, "UDP send returned a partial datagram.\n");
        return false;
    }
    return true;
}

static bool register_relay(int socket_fd, const struct options *options)
{
    char hello[160];
    int length;
    if (options->sending)
        length = snprintf(hello, sizeof hello, "HELLO %s send %.17g %.17g %.17g",
                          options->session, options->loss, options->corrupt, options->duplicate);
    else length = snprintf(hello, sizeof hello, "HELLO %s recv", options->session);
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        if (!send_bytes(socket_fd, (const uint8_t *)hello, (size_t)length)) return false;
        uint64_t now;
        if (!monotonic_ms(&now)) return false;
        uint64_t deadline = now + 1000;
        for (;;) {
            int ready = wait_until(socket_fd, deadline);
            if (ready < 0) return false;
            if (ready == 0) break;
            char reply[256];
            ssize_t count = recv(socket_fd, reply, sizeof reply - 1, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { perror("relay reply"); return false; }
            reply[(size_t)count] = '\0';
            if (count == 2 && memcmp(reply, "OK", 2) == 0) return true;
            if (count >= 4 && memcmp(reply, "ERR ", 4) == 0) {
                fprintf(stderr, "Relay refused registration: %s\n", reply + 4);
                return false;
            }
            /* Ignore stale binary traffic while awaiting the textual reply. */
        }
        fprintf(stderr, "No relay reply (attempt %u/5).\n", attempt + 1);
    }
    fprintf(stderr, "Relay registration timed out after five attempts.\n");
    return false;
}

static bool send_packet(int socket_fd, const rdt_packet *packet)
{
    uint8_t bytes[RDT_MAX_PACKET_SIZE];
    size_t length = rdt_packet_encode(packet, bytes, sizeof bytes);
    if (length == 0) {
        fprintf(stderr, "Internal packet encoding error.\n");
        return false;
    }
    return send_bytes(socket_fd, bytes, length);
}

/* An extra byte detects oversized datagrams even if recv truncates them. */
static int receive_packet(int socket_fd, rdt_packet *packet)
{
    uint8_t bytes[RDT_MAX_PACKET_SIZE + 1];
    ssize_t count = recv(socket_fd, bytes, sizeof bytes, 0);
    if (count < 0 && errno == EINTR) return 0;
    if (count < 0) { perror("recv"); return -1; }
    return rdt_packet_decode(bytes, (size_t)count, packet) ? 1 : 0;
}

static bool read_input(const char *path, uint8_t **bytes, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) { perror(path); return false; }
    bool success = false;
    *bytes = NULL;
    if (fseek(file, 0, SEEK_END) != 0) { perror("seek input"); goto cleanup; }
    long size = ftell(file);
    if (size < 0) { perror("input size"); goto cleanup; }
    if ((unsigned long)size > RDT_MAX_FILE_SIZE) {
        fprintf(stderr, "Input file exceeds the 16 MiB limit.\n");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_SET) != 0) { perror("rewind input"); goto cleanup; }
    *length = (size_t)size;
    *bytes = malloc(*length == 0 ? 1 : *length);
    if (*bytes == NULL) { perror("allocate input"); goto cleanup; }
    if (fread(*bytes, 1, *length, file) != *length || ferror(file)) {
        fprintf(stderr, "Could not read the complete input file.\n");
        goto cleanup;
    }
    if (fgetc(file) != EOF || ferror(file)) {
        fprintf(stderr, "Input file changed while being read.\n");
        goto cleanup;
    }
    success = true;
cleanup:
    if (fclose(file) != 0) { perror("close input"); success = false; }
    if (!success) { free(*bytes); *bytes = NULL; }
    return success;
}

static int run_sender(int socket_fd, const struct options *options,
                      const uint8_t *bytes, size_t length)
{
    gbn_sender sender;
    gbn_batch batch;
    if (!gbn_sender_init(&sender, bytes, length, options->window, options->timeout_ms))
        return 2;
    while (sender.status == RDT_ACTIVE) {
        uint64_t now;
        if (!monotonic_ms(&now)) return 2;
        gbn_sender_send(&sender, now, &batch);
        for (size_t i = 0; i < batch.count; ++i)
            if (!send_packet(socket_fd, &batch.packets[i])) return 2;
        int ready = wait_until(socket_fd, sender.deadline_ms);
        if (ready < 0 || !monotonic_ms(&now)) return 2;
        if (ready > 0) {
            rdt_packet ack;
            int received = receive_packet(socket_fd, &ack);
            if (received < 0) return 2;
            if (received > 0) gbn_sender_ack(&sender, &ack, now);
        }
        /* Check time even after bad or duplicate ACKs: they must not starve it. */
        if (!monotonic_ms(&now)) return 2;
        gbn_sender_timeout(&sender, now, &batch);
        for (size_t i = 0; i < batch.count; ++i)
            if (!send_packet(socket_fd, &batch.packets[i])) return 2;
    }
    if (sender.status != RDT_DONE) {
        fprintf(stderr, "Transfer gave up after 10 consecutive timeouts without progress.\n");
        return 2;
    }
    printf("Sent %zu bytes successfully.\n", length);
    return 0;
}

static int run_receiver(int socket_fd, FILE **output)
{
    uint64_t now;
    if (!monotonic_ms(&now)) return 2;
    gbn_receiver receiver;
    gbn_receiver_init(&receiver, now);
    size_t delivered = 0;
    while (receiver.status == RDT_ACTIVE) {
        uint64_t deadline = receiver.finishing ? receiver.linger_until_ms :
                            receiver.last_activity_ms + RDT_RECEIVER_IDLE_MS;
        int ready = wait_until(socket_fd, deadline);
        if (ready < 0 || !monotonic_ms(&now)) return 2;
        gbn_receiver_tick(&receiver, now);
        if (receiver.status != RDT_ACTIVE) break;
        if (ready == 0) continue;
        rdt_packet packet;
        int received = receive_packet(socket_fd, &packet);
        if (received < 0) return 2;
        if (received == 0) continue;
        gbn_receive_action action;
        gbn_receiver_receive(&receiver, &packet, now, &action);
        if (action.size > 0) {
            if (action.size > RDT_MAX_FILE_SIZE - delivered) {
                fprintf(stderr, "Received file exceeds the 16 MiB limit.\n");
                return 2;
            }
            if (fwrite(action.deliver, 1, action.size, *output) != action.size) {
                perror("write output");
                return 2;
            }
            delivered += action.size;
        }
        if (action.finish) {
            int closed = fclose(*output);
            *output = NULL;
            if (closed != 0) { perror("close output"); return 2; }
        }
        if (action.ack && !send_packet(socket_fd, &action.packet)) return 2;
    }
    if (receiver.status != RDT_DONE) {
        fprintf(stderr, "Receiver timed out or exceeded the 16 MiB file limit.\n");
        return 2;
    }
    printf("Received %zu bytes successfully.\n", delivered);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1) { usage(stdout); return 0; }
    struct options options;
    if (!parse_options(argc, argv, &options)) { usage(stderr); return 1; }
    uint8_t *bytes = NULL;
    size_t length = 0;
    FILE *output = NULL;
    int socket_fd = -1, result = 2;
    if (options.sending && !read_input(options.file, &bytes, &length)) goto cleanup;
    socket_fd = connect_relay(&options);
    if (socket_fd < 0) goto cleanup;
    if (!register_relay(socket_fd, &options)) goto cleanup;
    if (options.sending) result = run_sender(socket_fd, &options, bytes, length);
    else {
        output = fopen(options.file, "wb");
        if (output == NULL) { perror(options.file); goto cleanup; }
        result = run_receiver(socket_fd, &output);
    }
cleanup:
    if (output != NULL && fclose(output) != 0) { perror("close output"); result = 2; }
    if (socket_fd >= 0) close(socket_fd);
    free(bytes);
    return result;
}

```

### tests/lab-test.c

```c
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "harness/unity.h"
#include "../src/lab.h"

/* All protocol time is injected; these tests use neither sockets nor sleeps. */
void setUp(void) {}
void tearDown(void) {}

static rdt_packet packet(uint8_t type, uint32_t seq, const void *data, uint16_t size)
{
    rdt_packet result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.seq = seq;
    result.length = size;
    if (size != 0 && data != NULL && size <= RDT_PAYLOAD_SIZE) {
        memcpy(result.payload, data, size);
    }
    return result;
}

static void checksum_wire(uint8_t *wire, size_t size)
{
    uint16_t checksum;
    wire[2] = 0;
    wire[3] = 0;
    checksum = rdt_checksum(wire, size);
    wire[2] = (uint8_t)(checksum >> 8);
    wire[3] = (uint8_t)checksum;
}

static void assert_packet(const rdt_packet *actual, uint8_t type,
                          uint32_t seq, const void *payload, uint16_t size)
{
    TEST_ASSERT_EQUAL_UINT8(type, actual->type);
    TEST_ASSERT_EQUAL_UINT32(seq, actual->seq);
    TEST_ASSERT_EQUAL_UINT16(size, actual->length);
    if (size != 0) {
        TEST_ASSERT_EQUAL_MEMORY(payload, actual->payload, size);
    }
}

static void acknowledge(gbn_sender *sender, uint32_t next_expected, uint64_t now)
{
    rdt_packet ack = packet(RDT_ACK, next_expected, NULL, 0);
    gbn_sender_ack(sender, &ack, now);
}

static void test_checksum_rfc1071_and_odd_lengths(void)
{
    const uint8_t rfc1071[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    const uint8_t odd[] = {0x01, 0x02, 0x03};
    const uint8_t carry[] = {0xff, 0xff, 0xff, 0xff};
    const uint8_t single[] = {0x80};
    TEST_ASSERT_EQUAL_HEX16(0x220d, rdt_checksum(rfc1071, sizeof(rfc1071)));
    TEST_ASSERT_EQUAL_HEX16(0xfbfd, rdt_checksum(odd, sizeof(odd)));
    TEST_ASSERT_EQUAL_HEX16(0x0000, rdt_checksum(carry, sizeof(carry)));
    TEST_ASSERT_EQUAL_HEX16(0x7fff, rdt_checksum(single, sizeof(single)));
    TEST_ASSERT_EQUAL_HEX16(0xffff, rdt_checksum(NULL, 0));
    TEST_ASSERT_EQUAL_HEX16(0, rdt_checksum(NULL, 1));
}

static void test_exact_wire_fixtures_and_network_order(void)
{
    const uint8_t data_fixture[] = {0x00, 0x00, 0x96, 0x91, 0x00, 0x00,
                                   0x00, 0x02, 0x00, 0x03, 0x48, 0x69, 0x21};
    const uint8_t ack_fixture[] = {0x01, 0x00, 0xfe, 0xfc, 0x00,
                                  0x00, 0x00, 0x03, 0x00, 0x00};
    uint8_t wire[RDT_MAX_PACKET_SIZE];
    rdt_packet data = packet(RDT_DATA, 2, "Hi!", 3);
    rdt_packet ack = packet(RDT_ACK, 3, NULL, 0);
    rdt_packet decoded;
    size_t size = rdt_packet_encode(&data, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_UINT(sizeof(data_fixture), size);
    TEST_ASSERT_EQUAL_MEMORY(data_fixture, wire, size);
    TEST_ASSERT_EQUAL_HEX16(0, rdt_checksum(wire, size));
    TEST_ASSERT_TRUE(rdt_packet_decode(wire, size, &decoded));
    assert_packet(&decoded, RDT_DATA, 2, "Hi!", 3);
    size = rdt_packet_encode(&ack, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_UINT(sizeof(ack_fixture), size);
    TEST_ASSERT_EQUAL_MEMORY(ack_fixture, wire, size);
    TEST_ASSERT_TRUE(rdt_packet_decode(ack_fixture, sizeof(ack_fixture), &decoded));
    assert_packet(&decoded, RDT_ACK, 3, NULL, 0);
    data = packet(RDT_DATA, UINT32_C(0x12345678), "Z", 1);
    TEST_ASSERT_EQUAL_UINT(11, rdt_packet_encode(&data, wire, sizeof(wire)));
    TEST_ASSERT_EQUAL_HEX8(0x12, wire[4]);
    TEST_ASSERT_EQUAL_HEX8(0x34, wire[5]);
    TEST_ASSERT_EQUAL_HEX8(0x56, wire[6]);
    TEST_ASSERT_EQUAL_HEX8(0x78, wire[7]);
    TEST_ASSERT_TRUE(rdt_packet_decode(wire, 11, &decoded));
    assert_packet(&decoded, RDT_DATA, UINT32_C(0x12345678), "Z", 1);
}

static void test_packet_roundtrips_boundary_payloads(void)
{
    uint8_t wire[RDT_MAX_PACKET_SIZE];
    rdt_packet original, decoded;
    original = packet(RDT_DATA, UINT32_MAX, NULL, RDT_PAYLOAD_SIZE);
    for (size_t i = 0; i < RDT_PAYLOAD_SIZE; ++i) {
        original.payload[i] = (uint8_t)(i * 37U);
    }
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_PACKET_SIZE, rdt_packet_encode(&original, wire, sizeof(wire)));
    TEST_ASSERT_TRUE(rdt_packet_decode(wire, sizeof(wire), &decoded));
    assert_packet(&decoded, RDT_DATA, UINT32_MAX, original.payload, RDT_PAYLOAD_SIZE);
    for (uint8_t type = RDT_DATA; type <= RDT_FIN; ++type) {
        original = packet(type, 0, NULL, 0);
        TEST_ASSERT_EQUAL_UINT(RDT_HEADER_SIZE, rdt_packet_encode(&original, wire, sizeof(wire)));
        TEST_ASSERT_TRUE(rdt_packet_decode(wire, RDT_HEADER_SIZE, &decoded));
        assert_packet(&decoded, type, 0, NULL, 0);
    }
}

static void test_packet_encoder_rejects_invalid_arguments(void)
{
    uint8_t wire[RDT_MAX_PACKET_SIZE];
    rdt_packet p = packet(RDT_DATA, 0, "x", 1);
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(NULL, wire, sizeof(wire)));
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, NULL, sizeof(wire)));
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, RDT_HEADER_SIZE));
    p.type = 3;
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, sizeof(wire)));
    p.type = RDT_DATA;
    p.length = RDT_PAYLOAD_SIZE + 1;
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, sizeof(wire)));
    p.length = 1;
    p.type = RDT_ACK;
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, sizeof(wire)));
    p.type = RDT_FIN;
    TEST_ASSERT_EQUAL_UINT(0, rdt_packet_encode(&p, wire, sizeof(wire)));
}

static void test_packet_decoder_rejects_each_malformed_wire_condition(void)
{
    uint8_t wire[RDT_MAX_PACKET_SIZE + 1];
    uint8_t valid[RDT_HEADER_SIZE + 3];
    rdt_packet p = packet(RDT_DATA, 2, "Hi!", 3), decoded;
    size_t size = rdt_packet_encode(&p, valid, sizeof(valid));
    TEST_ASSERT_FALSE(rdt_packet_decode(NULL, size, &decoded));
    TEST_ASSERT_FALSE(rdt_packet_decode(valid, size, NULL));
    for (size_t truncated = 0; truncated < RDT_HEADER_SIZE; ++truncated) {
        TEST_ASSERT_FALSE(rdt_packet_decode(valid, truncated, &decoded));
    }
    TEST_ASSERT_FALSE(rdt_packet_decode(valid, size - 1, &decoded));
    memcpy(wire, valid, size);
    wire[size] = 0;
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, size + 1, &decoded));
    memcpy(wire, valid, size);
    wire[0] = 3;
    checksum_wire(wire, size);
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    memcpy(wire, valid, size);
    wire[1] = 1;
    checksum_wire(wire, size);
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    memcpy(wire, valid, size);
    wire[8] = 4;
    wire[9] = 1;
    checksum_wire(wire, size);
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    for (uint8_t type = RDT_ACK; type <= RDT_FIN; ++type) {
        memcpy(wire, valid, size);
        wire[0] = type;
        checksum_wire(wire, size);
        TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    }
    for (size_t index = 0; index < size; ++index) {
        memcpy(wire, valid, size);
        wire[index] ^= 1;
        TEST_ASSERT_FALSE(rdt_packet_decode(wire, size, &decoded));
    }
    memset(wire, 0, sizeof(wire));
    wire[8] = 4;
    wire[9] = 1;
    checksum_wire(wire, sizeof(wire));
    TEST_ASSERT_FALSE(rdt_packet_decode(wire, sizeof(wire), &decoded));
}

static void test_sender_initialization_limits(void)
{
    uint8_t byte = 0;
    gbn_sender sender;
    TEST_ASSERT_FALSE(gbn_sender_init(NULL, &byte, 1, 1, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, NULL, 1, 1, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, &byte, RDT_MAX_FILE_SIZE + 1U, 1, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, &byte, 1, 0, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, &byte, 1, RDT_MAX_WINDOW + 1U, 100));
    TEST_ASSERT_FALSE(gbn_sender_init(&sender, &byte, 1, 1, 0));
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, NULL, 0, 1, 1));
    TEST_ASSERT_EQUAL_UINT(0, sender.data_count);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, sender.status);
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, &byte, RDT_MAX_FILE_SIZE, RDT_MAX_WINDOW, 100));
    TEST_ASSERT_EQUAL_UINT32(RDT_MAX_FILE_SIZE / RDT_PAYLOAD_SIZE, sender.data_count);
}

static void test_sender_window_slides_with_cumulative_ack(void)
{
    uint8_t bytes[4 * RDT_PAYLOAD_SIZE + 7];
    gbn_sender sender;
    gbn_batch batch;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)i;
    }
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 3, 100));
    TEST_ASSERT_EQUAL_UINT32(5, sender.data_count);
    gbn_sender_send(&sender, 10, &batch);
    TEST_ASSERT_EQUAL_UINT(3, batch.count);
    for (uint32_t i = 0; i < 3; ++i) {
        assert_packet(&batch.packets[i], RDT_DATA, i, bytes + i * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    }
    TEST_ASSERT_EQUAL_UINT32(0, sender.base);
    TEST_ASSERT_EQUAL_UINT32(3, sender.next);
    TEST_ASSERT_EQUAL_UINT64(110, sender.deadline_ms);
    gbn_sender_send(&sender, 20, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    TEST_ASSERT_EQUAL_UINT64(110, sender.deadline_ms);
    acknowledge(&sender, 2, 40);
    TEST_ASSERT_EQUAL_UINT32(2, sender.base);
    TEST_ASSERT_EQUAL_UINT64(140, sender.deadline_ms);
    gbn_sender_send(&sender, 41, &batch);
    TEST_ASSERT_EQUAL_UINT(2, batch.count);
    assert_packet(&batch.packets[0], RDT_DATA, 3, bytes + 3 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    assert_packet(&batch.packets[1], RDT_DATA, 4, bytes + 4 * RDT_PAYLOAD_SIZE, 7);
    TEST_ASSERT_EQUAL_UINT64(140, sender.deadline_ms);
    acknowledge(&sender, 4, 60);
    gbn_sender_send(&sender, 61, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count); /* FIN waits for the last DATA ACK. */
    acknowledge(&sender, 5, 80);
    gbn_sender_send(&sender, 81, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 5, NULL, 0);
    TEST_ASSERT_EQUAL_UINT64(181, sender.deadline_ms);
    acknowledge(&sender, 6, 90);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, sender.status);
    gbn_sender_send(&sender, 100, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
}

static void test_sender_empty_file_and_exact_payload_multiple(void)
{
    uint8_t bytes[2 * RDT_PAYLOAD_SIZE] = {0};
    gbn_sender sender;
    gbn_batch batch;
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, NULL, 0, 64, 100));
    gbn_sender_send(&sender, 0, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 0, NULL, 0);
    acknowledge(&sender, 1, 1);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, sender.status);
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 64, 100));
    gbn_sender_send(&sender, 0, &batch);
    TEST_ASSERT_EQUAL_UINT(2, batch.count);
    TEST_ASSERT_EQUAL_UINT16(RDT_PAYLOAD_SIZE, batch.packets[0].length);
    TEST_ASSERT_EQUAL_UINT16(RDT_PAYLOAD_SIZE, batch.packets[1].length);
    acknowledge(&sender, 2, 1);
    gbn_sender_send(&sender, 1, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 2, NULL, 0);
}

static void test_sender_ignores_duplicate_future_and_non_ack_packets(void)
{
    uint8_t bytes[3 * RDT_PAYLOAD_SIZE] = {0};
    gbn_sender sender;
    gbn_batch batch;
    rdt_packet wrong;
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 2, 100));
    gbn_sender_send(&sender, 5, &batch);
    acknowledge(&sender, 0, 50);
    acknowledge(&sender, 3, 50);
    acknowledge(&sender, UINT32_MAX, 50);
    wrong = packet(RDT_DATA, 1, "x", 1);
    gbn_sender_ack(&sender, &wrong, 50);
    wrong = packet(RDT_FIN, 1, NULL, 0);
    gbn_sender_ack(&sender, &wrong, 50);
    wrong = packet(RDT_ACK, 1, "x", 1);
    gbn_sender_ack(&sender, &wrong, 50);
    TEST_ASSERT_EQUAL_UINT32(0, sender.base);
    TEST_ASSERT_EQUAL_UINT64(105, sender.deadline_ms);
    acknowledge(&sender, 1, 60);
    acknowledge(&sender, 1, 90);
    acknowledge(&sender, 0, 90);
    TEST_ASSERT_EQUAL_UINT32(1, sender.base);
    TEST_ASSERT_EQUAL_UINT64(160, sender.deadline_ms);
}

static void test_sender_timer_retransmits_entire_unacknowledged_window(void)
{
    uint8_t bytes[3 * RDT_PAYLOAD_SIZE + 1] = {0};
    gbn_sender sender;
    gbn_batch first, resent;
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 3, 100));
    gbn_sender_timeout(&sender, 10000, &resent);
    TEST_ASSERT_EQUAL_UINT(0, resent.count);
    gbn_sender_send(&sender, 20, &first);
    gbn_sender_timeout(&sender, 119, &resent);
    TEST_ASSERT_EQUAL_UINT(0, resent.count);
    TEST_ASSERT_EQUAL_UINT(0, sender.consecutive_timeouts);
    gbn_sender_timeout(&sender, 120, &resent);
    TEST_ASSERT_EQUAL_UINT(3, resent.count);
    TEST_ASSERT_EQUAL_UINT(1, sender.consecutive_timeouts);
    TEST_ASSERT_EQUAL_UINT64(220, sender.deadline_ms);
    for (size_t i = 0; i < first.count; ++i) {
        assert_packet(&resent.packets[i], first.packets[i].type, first.packets[i].seq,
                      first.packets[i].payload, first.packets[i].length);
    }
    acknowledge(&sender, 2, 150);
    TEST_ASSERT_EQUAL_UINT(0, sender.consecutive_timeouts);
    gbn_sender_timeout(&sender, 249, &resent);
    TEST_ASSERT_EQUAL_UINT(0, resent.count);
    gbn_sender_timeout(&sender, 250, &resent);
    TEST_ASSERT_EQUAL_UINT(1, resent.count);
    assert_packet(&resent.packets[0], RDT_DATA, 2, bytes + 2 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
}

static void test_sender_fails_after_ten_consecutive_timeouts_and_resets_on_progress(void)
{
    uint8_t bytes[2 * RDT_PAYLOAD_SIZE] = {0};
    gbn_sender sender;
    gbn_batch batch;
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 2, 10));
    gbn_sender_send(&sender, 0, &batch);
    for (unsigned i = 1; i < RDT_MAX_TIMEOUTS; ++i) {
        gbn_sender_timeout(&sender, i * 10U, &batch);
        TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, sender.status);
        TEST_ASSERT_EQUAL_UINT(2, batch.count);
    }
    acknowledge(&sender, 1, 95);
    TEST_ASSERT_EQUAL_UINT(0, sender.consecutive_timeouts);
    acknowledge(&sender, 1, 100);
    for (unsigned i = 1; i <= RDT_MAX_TIMEOUTS; ++i) {
        gbn_sender_timeout(&sender, 95U + i * 10U, &batch);
        TEST_ASSERT_EQUAL_UINT(i, sender.consecutive_timeouts);
        if (i < RDT_MAX_TIMEOUTS) {
            TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, sender.status);
            TEST_ASSERT_EQUAL_UINT(1, batch.count);
        } else {
            TEST_ASSERT_EQUAL_INT(RDT_FAILED, sender.status);
            TEST_ASSERT_EQUAL_UINT(0, batch.count);
        }
    }
    acknowledge(&sender, 2, 200);
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, sender.status);
    gbn_sender_send(&sender, 200, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_timeout(&sender, 200, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
}

static void test_sender_fin_retransmission_and_ring_buffer_reuse(void)
{
    uint8_t bytes[5 * RDT_PAYLOAD_SIZE];
    gbn_sender sender;
    gbn_batch batch;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)(i / RDT_PAYLOAD_SIZE + 17U);
    }
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, bytes, sizeof(bytes), 2, 10));
    gbn_sender_send(&sender, 0, &batch);
    acknowledge(&sender, 2, 1);
    gbn_sender_send(&sender, 2, &batch);
    acknowledge(&sender, 3, 3);
    gbn_sender_send(&sender, 4, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_DATA, 4, bytes + 4 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    gbn_sender_timeout(&sender, 13, &batch);
    TEST_ASSERT_EQUAL_UINT(2, batch.count);
    assert_packet(&batch.packets[0], RDT_DATA, 3, bytes + 3 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    assert_packet(&batch.packets[1], RDT_DATA, 4, bytes + 4 * RDT_PAYLOAD_SIZE, RDT_PAYLOAD_SIZE);
    acknowledge(&sender, 5, 14);
    gbn_sender_send(&sender, 15, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 5, NULL, 0);
    gbn_sender_send(&sender, 16, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_timeout(&sender, 25, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    assert_packet(&batch.packets[0], RDT_FIN, 5, NULL, 0);
    acknowledge(&sender, 6, 26);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, sender.status);
    gbn_sender_timeout(&sender, 100, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
}

static void test_receiver_delivers_in_order_and_repeats_cumulative_ack(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_DATA, 1, "second", 6);
    gbn_receiver_init(&receiver, 10);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, receiver.status);
    gbn_receiver_receive(&receiver, &p, 20, &action);
    TEST_ASSERT_TRUE(action.ack);
    assert_packet(&action.packet, RDT_ACK, 0, NULL, 0);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_EQUAL_UINT(0, action.size);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_UINT32(0, receiver.expected);
    p = packet(RDT_DATA, 0, "first", 5);
    gbn_receiver_receive(&receiver, &p, 30, &action);
    TEST_ASSERT_TRUE(action.ack);
    assert_packet(&action.packet, RDT_ACK, 1, NULL, 0);
    TEST_ASSERT_EQUAL_MEMORY("first", action.deliver, action.size);
    TEST_ASSERT_EQUAL_UINT(5, action.size);
    TEST_ASSERT_EQUAL_UINT(5, receiver.received_size);
    TEST_ASSERT_EQUAL_UINT64(30, receiver.last_activity_ms);
    gbn_receiver_receive(&receiver, &p, 40, &action);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(1, action.packet.seq);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_EQUAL_UINT(5, receiver.received_size);
    p = packet(RDT_DATA, 1, "second", 6);
    gbn_receiver_receive(&receiver, &p, 50, &action);
    TEST_ASSERT_EQUAL_UINT32(2, action.packet.seq);
    TEST_ASSERT_EQUAL_MEMORY("second", action.deliver, 6);
    TEST_ASSERT_EQUAL_UINT(11, receiver.received_size);
}

static void test_receiver_fin_must_be_in_order_and_linger_is_fixed(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_FIN, 1, NULL, 0);
    gbn_receiver_init(&receiver, 0);
    gbn_receiver_receive(&receiver, &p, 10, &action);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(0, action.packet.seq);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_FALSE(receiver.finishing);
    p = packet(RDT_DATA, 0, "abc", 3);
    gbn_receiver_receive(&receiver, &p, 20, &action);
    p = packet(RDT_FIN, 1, NULL, 0);
    gbn_receiver_receive(&receiver, &p, 30, &action);
    TEST_ASSERT_TRUE(action.finish);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(2, action.packet.seq);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_TRUE(receiver.finishing);
    TEST_ASSERT_EQUAL_UINT64(30 + RDT_LINGER_MS, receiver.linger_until_ms);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, 2029));
    gbn_receiver_receive(&receiver, &p, 2029, &action);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(2, action.packet.seq);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_UINT64(2030, receiver.linger_until_ms);
    p = packet(RDT_DATA, 1, "ignored", 7);
    gbn_receiver_receive(&receiver, &p, 2029, &action);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_UINT(3, receiver.received_size);
    p = packet(RDT_FIN, 2, NULL, 0);
    gbn_receiver_receive(&receiver, &p, 2029, &action);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, gbn_receiver_tick(&receiver, 2030));
    gbn_receiver_receive(&receiver, &p, 2031, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, gbn_receiver_tick(&receiver, 99999));
}

static void test_receiver_empty_file_and_inactivity_boundary(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_FIN, 0, NULL, 0);
    gbn_receiver_init(&receiver, 100);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, 100 + RDT_RECEIVER_IDLE_MS - 1));
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, gbn_receiver_tick(&receiver, 100 + RDT_RECEIVER_IDLE_MS));
    gbn_receiver_receive(&receiver, &p, 30101, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, gbn_receiver_tick(&receiver, 100000));
    gbn_receiver_init(&receiver, 0);
    gbn_receiver_receive(&receiver, &p, 1, &action);
    TEST_ASSERT_TRUE(action.finish);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT32(1, action.packet.seq);
    TEST_ASSERT_EQUAL_UINT(0, receiver.received_size);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, gbn_receiver_tick(&receiver, 1 + RDT_LINGER_MS));
}

static void test_receiver_valid_activity_resets_idle_timer(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_DATA, 2, "gap", 3);
    gbn_receiver_init(&receiver, 100);
    gbn_receiver_receive(&receiver, &p, 30099, &action);
    TEST_ASSERT_TRUE(action.ack);
    TEST_ASSERT_EQUAL_UINT64(30099, receiver.last_activity_ms);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, 60098));
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, gbn_receiver_tick(&receiver, 60099));
}

static void test_receiver_rejects_invalid_semantic_packets(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_ACK, 0, NULL, 0);
    gbn_receiver_init(&receiver, 100);
    gbn_receiver_receive(&receiver, &p, 200, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_EQUAL_UINT64(100, receiver.last_activity_ms);
    p.type = 3;
    gbn_receiver_receive(&receiver, &p, 200, &action);
    TEST_ASSERT_FALSE(action.ack);
    p.type = RDT_DATA;
    p.length = RDT_PAYLOAD_SIZE + 1;
    gbn_receiver_receive(&receiver, &p, 200, &action);
    TEST_ASSERT_FALSE(action.ack);
    p.type = RDT_FIN;
    p.length = 1;
    gbn_receiver_receive(&receiver, &p, 200, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_FALSE(action.finish);
    TEST_ASSERT_EQUAL_UINT64(100, receiver.last_activity_ms);
}

static void test_receiver_refuses_file_above_limit(void)
{
    gbn_receiver receiver;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_DATA, 0, "x", 1);
    gbn_receiver_init(&receiver, 0);
    receiver.received_size = RDT_MAX_FILE_SIZE;
    gbn_receiver_receive(&receiver, &p, 1, &action);
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, receiver.status);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_FILE_SIZE, receiver.received_size);
    gbn_receiver_init(&receiver, 0);
    receiver.received_size = RDT_MAX_FILE_SIZE - 1;
    gbn_receiver_receive(&receiver, &p, 1, &action);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, receiver.status);
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_FILE_SIZE, receiver.received_size);
    TEST_ASSERT_EQUAL_UINT(1, action.size);
}

static void test_public_state_machine_null_arguments_are_safe(void)
{
    gbn_sender sender;
    gbn_receiver receiver;
    gbn_batch batch;
    gbn_receive_action action;
    rdt_packet p = packet(RDT_FIN, 0, NULL, 0);
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, NULL, 0, 1, 10));
    gbn_sender_send(&sender, 0, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, sender.next);
    batch.count = 7;
    gbn_sender_send(NULL, 0, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_timeout(&sender, 0, NULL);
    batch.count = 7;
    gbn_sender_timeout(NULL, 0, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_ack(NULL, &p, 0);
    gbn_sender_ack(&sender, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(0, sender.base);

    gbn_receiver_init(NULL, 0);
    TEST_ASSERT_EQUAL_INT(RDT_FAILED, gbn_receiver_tick(NULL, 0));
    gbn_receiver_init(&receiver, 0);
    gbn_receiver_receive(&receiver, &p, 0, NULL);
    TEST_ASSERT_FALSE(receiver.finishing);
    memset(&action, 0xff, sizeof(action));
    gbn_receiver_receive(NULL, &p, 0, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
    TEST_ASSERT_FALSE(action.finish);
    memset(&action, 0xff, sizeof(action));
    gbn_receiver_receive(&receiver, NULL, 0, &action);
    TEST_ASSERT_FALSE(action.ack);
    TEST_ASSERT_NULL(action.deliver);
}

static void test_virtual_clock_boundaries_do_not_wrap(void)
{
    gbn_sender sender;
    gbn_receiver receiver;
    gbn_batch batch;
    gbn_receive_action action;
    rdt_packet fin = packet(RDT_FIN, 0, NULL, 0);
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, NULL, 0, 1, 10));
    gbn_sender_send(&sender, UINT64_MAX - 5U, &batch);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, sender.deadline_ms);
    gbn_sender_timeout(&sender, UINT64_MAX - 1U, &batch);
    TEST_ASSERT_EQUAL_UINT(0, batch.count);
    gbn_sender_timeout(&sender, UINT64_MAX, &batch);
    TEST_ASSERT_EQUAL_UINT(1, batch.count);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, sender.deadline_ms);

    gbn_receiver_init(&receiver, 100);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, 99));
    gbn_receiver_init(&receiver, UINT64_MAX - 1000U);
    gbn_receiver_receive(&receiver, &fin, UINT64_MAX - 1000U, &action);
    TEST_ASSERT_TRUE(action.finish);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, receiver.linger_until_ms);
    TEST_ASSERT_EQUAL_INT(RDT_ACTIVE, gbn_receiver_tick(&receiver, UINT64_MAX - 1U));
    TEST_ASSERT_EQUAL_INT(RDT_DONE, gbn_receiver_tick(&receiver, UINT64_MAX));
}

/* Loss, corruption and duplication are selected independently with 20%
 * probability in each direction, including FIN and ACK. Delivery is delayed
 * by a few virtual milliseconds, also exercising reordering and stale ACKs. */
#define CHANNEL_CAPACITY 4096U
typedef struct {
    bool to_receiver;
    uint64_t due;
    size_t size;
    uint8_t wire[RDT_MAX_PACKET_SIZE];
} channel_event;
typedef struct {
    channel_event *events;
    size_t count;
    uint32_t random;
    unsigned dropped[2], corrupted[2], duplicated[2];
} memory_channel;

static uint32_t next_random(memory_channel *channel)
{
    uint32_t value = channel->random;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    channel->random = value;
    return value;
}

static void channel_send(memory_channel *channel, const rdt_packet *p,
                         bool to_receiver, uint64_t now, bool impair)
{
    channel_event event;
    unsigned direction = to_receiver ? 0U : 1U;
    bool drop = impair && next_random(channel) % 5U == 0;
    bool corrupt = impair && next_random(channel) % 5U == 0;
    bool duplicate = impair && next_random(channel) % 5U == 0;
    event.to_receiver = to_receiver;
    event.due = now + 1U + next_random(channel) % 4U;
    event.size = rdt_packet_encode(p, event.wire, sizeof(event.wire));
    TEST_ASSERT_NOT_EQUAL(0, event.size);
    if (drop) {
        ++channel->dropped[direction];
        return;
    }
    if (corrupt) {
        ++channel->corrupted[direction];
        event.wire[next_random(channel) % event.size] ^= 0x40;
    }
    TEST_ASSERT_LESS_THAN(CHANNEL_CAPACITY, channel->count);
    channel->events[channel->count++] = event;
    if (duplicate) {
        ++channel->duplicated[direction];
        event.due += 1;
        TEST_ASSERT_LESS_THAN(CHANNEL_CAPACITY, channel->count);
        channel->events[channel->count++] = event;
    }
}

static void simulate_transfer(uint32_t seed, size_t size, size_t window,
                              bool impair, bool check_damage)
{
    uint8_t *input = malloc(size == 0 ? 1 : size);
    uint8_t *output = malloc(size == 0 ? 1 : size);
    memory_channel channel;
    gbn_sender sender;
    gbn_receiver receiver;
    gbn_batch batch;
    size_t delivered = 0;
    unsigned finishes = 0;
    memset(&channel, 0, sizeof(channel));
    channel.events = calloc(CHANNEL_CAPACITY, sizeof(*channel.events));
    channel.random = seed;
    TEST_ASSERT_NOT_NULL(input);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_NOT_NULL(channel.events);
    for (size_t i = 0; i < size; ++i) {
        input[i] = (uint8_t)(i * 37U + i / 251U);
    }
    TEST_ASSERT_TRUE(gbn_sender_init(&sender, input, size, window, 100));
    gbn_receiver_init(&receiver, 0);
    for (uint64_t now = 0; now < 120000; ++now) {
        gbn_sender_send(&sender, now, &batch);
        for (size_t i = 0; i < batch.count; ++i) {
            channel_send(&channel, &batch.packets[i], true, now, impair);
        }
        gbn_sender_timeout(&sender, now, &batch);
        for (size_t i = 0; i < batch.count; ++i) {
            channel_send(&channel, &batch.packets[i], true, now, impair);
        }
        for (size_t i = 0; i < channel.count;) {
            channel_event event;
            rdt_packet decoded;
            if (channel.events[i].due > now) {
                ++i;
                continue;
            }
            event = channel.events[i];
            channel.events[i] = channel.events[--channel.count];
            if (!rdt_packet_decode(event.wire, event.size, &decoded)) {
                continue;
            }
            if (event.to_receiver) {
                gbn_receive_action action;
                gbn_receiver_receive(&receiver, &decoded, now, &action);
                if (action.deliver != NULL) {
                    TEST_ASSERT_LESS_OR_EQUAL(size, delivered + action.size);
                    memcpy(output + delivered, action.deliver, action.size);
                    delivered += action.size;
                }
                if (action.finish) {
                    ++finishes;
                }
                if (action.ack) {
                    channel_send(&channel, &action.packet, false, now, impair);
                }
            } else {
                gbn_sender_ack(&sender, &decoded, now);
            }
        }
        gbn_receiver_tick(&receiver, now);
        if (sender.status != RDT_ACTIVE && receiver.status != RDT_ACTIVE) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(RDT_DONE, sender.status, "fixed-seed sender did not complete");
    TEST_ASSERT_EQUAL_INT_MESSAGE(RDT_DONE, receiver.status, "fixed-seed receiver did not complete");
    TEST_ASSERT_EQUAL_UINT(size, delivered);
    TEST_ASSERT_EQUAL_UINT(1, finishes);
    if (size != 0) {
        TEST_ASSERT_EQUAL_MEMORY(input, output, size);
    }
    if (check_damage) {
        for (unsigned i = 0; i < 2; ++i) {
            TEST_ASSERT_GREATER_THAN(0, channel.dropped[i]);
            TEST_ASSERT_GREATER_THAN(0, channel.corrupted[i]);
            TEST_ASSERT_GREATER_THAN(0, channel.duplicated[i]);
        }
    }
    free(channel.events);
    free(output);
    free(input);
}

static void test_in_memory_channel_clean_empty_and_exact_multiple_files(void)
{
    simulate_transfer(1, 0, 1, false, false);
    simulate_transfer(2, 1, 1, false, false);
    simulate_transfer(3, 1024, 4, false, false);
    simulate_transfer(4, 2048, 64, false, false);
    simulate_transfer(5, 3 * 1024 + 7, 2, false, false);
}

static void test_in_memory_channel_fixed_seed_bidirectional_damage(void)
{
    simulate_transfer(UINT32_C(0x12345678), 32 * 1024 + 19, 8, true, true);
    simulate_transfer(UINT32_C(0x9abcdef0), 48 * 1024, 16, true, true);
    simulate_transfer(UINT32_C(0xdeadbeef), 64 * 1024 + 3, 64, true, true);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_checksum_rfc1071_and_odd_lengths);
    RUN_TEST(test_exact_wire_fixtures_and_network_order);
    RUN_TEST(test_packet_roundtrips_boundary_payloads);
    RUN_TEST(test_packet_encoder_rejects_invalid_arguments);
    RUN_TEST(test_packet_decoder_rejects_each_malformed_wire_condition);
    RUN_TEST(test_sender_initialization_limits);
    RUN_TEST(test_sender_window_slides_with_cumulative_ack);
    RUN_TEST(test_sender_empty_file_and_exact_payload_multiple);
    RUN_TEST(test_sender_ignores_duplicate_future_and_non_ack_packets);
    RUN_TEST(test_sender_timer_retransmits_entire_unacknowledged_window);
    RUN_TEST(test_sender_fails_after_ten_consecutive_timeouts_and_resets_on_progress);
    RUN_TEST(test_sender_fin_retransmission_and_ring_buffer_reuse);
    RUN_TEST(test_receiver_delivers_in_order_and_repeats_cumulative_ack);
    RUN_TEST(test_receiver_fin_must_be_in_order_and_linger_is_fixed);
    RUN_TEST(test_receiver_empty_file_and_inactivity_boundary);
    RUN_TEST(test_receiver_valid_activity_resets_idle_timer);
    RUN_TEST(test_receiver_rejects_invalid_semantic_packets);
    RUN_TEST(test_receiver_refuses_file_above_limit);
    RUN_TEST(test_public_state_machine_null_arguments_are_safe);
    RUN_TEST(test_virtual_clock_boundaries_do_not_wrap);
    RUN_TEST(test_in_memory_channel_clean_empty_and_exact_multiple_files);
    RUN_TEST(test_in_memory_channel_fixed_seed_bidirectional_damage);
    return UNITY_END();
}

```

## End of Report

All commands above completed successfully.

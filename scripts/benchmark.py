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

from transfer_support import DEFAULT_RELAY, transfer


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
    parser.add_argument("--relay", help="relay script path; default is the instructor cs425_relay.py")
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
    relay = Path(args.relay).resolve() if args.relay else DEFAULT_RELAY
    relay_kind = ("instructor" if relay == DEFAULT_RELAY else
                  "compatibility" if relay == Path(__file__).with_name("cs425_relay.py").resolve()
                  else "custom")
    metadata = {
        "measured_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": platform.platform(), "machine": platform.machine(),
        "python": platform.python_version(), "jobs": args.jobs, "trials": args.trials,
        "bytes": args.size, "input_source": "os.urandom (operating-system random bytes)",
        "input_sha256": hashlib.sha256(payload).hexdigest(),
        "app": str(app), "app_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
        "relay": str(relay), "relay_sha256": hashlib.sha256(relay.read_bytes()).hexdigest(),
        "relay_kind": relay_kind,
        "bundled_compatibility_relay": relay_kind == "compatibility",
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
    report += [f"Relay: **{relay_kind}** (`{relay}`). "
               "Its SHA-256 is recorded in `environment.json`.", ""]
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

#!/usr/bin/env python3
"""Run bounded local file-transfer tests using the instructor relay."""

import argparse
from pathlib import Path
import subprocess
import sys

from transfer_support import transfer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", default="build/release/myapp")
    parser.add_argument("--output", default="build/integration")
    parser.add_argument("--relay", help="relay script path (default: instructor cs425_relay.py)")
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
            ("small-decimals", b"decimal registration\x00\xff",
             {"loss": "0.00001", "corrupt": "0.000001", "duplicate": "0.0000001"}),
            ("decimal-spelling", b"decimal spellings",
             {"loss": ".0", "corrupt": "00.000", "duplicate": ".5"}),
            ("window-one", bytes(range(256)) * 12, {"window": 1, "loss": 0.1}),
            ("window-max", bytes(range(256)) * 100, {"window": 64, "loss": 0.1}),
        ]
    try:
        for name, payload, options in cases:
            result = transfer(app, Path(args.output) / name, payload,
                              session=f"test-{name}", deadline=60,
                              relay_script=args.relay, **options)
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

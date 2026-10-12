"""Bounded subprocess helpers shared by the local tests and benchmark."""

import os
from pathlib import Path
import socket
import shlex
import signal
import subprocess
import sys
import time


DEFAULT_RELAY = Path(__file__).resolve().parent.parent / "cs425_relay.py"


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
    relay_script = str(Path(relay_script or DEFAULT_RELAY).resolve())
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

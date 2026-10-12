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


def registration_size_limit(app, directory):
    # Preserved rate text must not truncate HELLO or read beyond its buffer.
    with ScriptedRelay(app, directory, extra=("-l", "0." + "0" * 2048)) as relay:
        _, err = relay.finish(expected=2)
        require("2048-byte limit" in err, "oversized HELLO lacks its reason")
        relay.expect_silence(timeout=0.05)


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
        for value in ("nan", "inf", "-inf", "1e309", "-0.1", "0.50001", ".", "oops",
                      "1e-5", "0x0.1p0", "0.1 ", "0.1\n", "0."):
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
             registration_size_limit,
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

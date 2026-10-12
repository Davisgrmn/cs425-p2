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

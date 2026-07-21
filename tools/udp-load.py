#!/usr/bin/env python3
import argparse
import socket
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser(description="ctunnel UDP echo load/smoke generator")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--size", type=int, default=64)
    parser.add_argument("--timeout", type=float, default=1.0)
    args = parser.parse_args()

    if args.count < 1 or args.size < 1:
        parser.error("--count and --size must be positive")

    family = socket.AF_INET6 if ":" in args.host else socket.AF_INET
    target = (args.host, args.port)
    if family == socket.AF_INET6:
        target = (args.host, args.port, 0, 0)
    sock = socket.socket(family, socket.SOCK_DGRAM)
    sock.settimeout(args.timeout)

    received = 0
    start = time.monotonic()
    for index in range(args.count):
        prefix = f"ctunnel-udp-{index:08d}:".encode()
        payload = (prefix + bytes([index & 0xFF]) * args.size)[: args.size]
        sock.sendto(payload, target)
        data, _ = sock.recvfrom(max(65535, args.size + 32))
        if data != b"echo:" + payload:
            raise RuntimeError(f"bad echo at packet {index}: {data[:64]!r}")
        received += 1

    elapsed = max(time.monotonic() - start, 0.000001)
    print(
        f"udp packets={received} bytes={received * args.size} "
        f"seconds={elapsed:.3f} pps={received / elapsed:.1f}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"udp-load: {exc}", file=sys.stderr)
        raise SystemExit(1)

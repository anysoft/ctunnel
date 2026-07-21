#!/usr/bin/env python3
import argparse
import concurrent.futures
import os
import random
import socket
import statistics
import sys
import threading
import time


def parse_size(text: str) -> int:
    units = {"k": 1024, "m": 1024 * 1024}
    text = text.strip().lower()
    if text[-1:] in units:
        return int(text[:-1]) * units[text[-1]]
    return int(text)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        part = sock.recv(min(65536, size - len(chunks)))
        if not part:
            raise RuntimeError(f"short read: got {len(chunks)} of {size}")
        chunks.extend(part)
    return bytes(chunks)


def one_connection(args, index: int):
    payload = (f"ctunnel-load-{index}-".encode() *
               ((args.bytes + 31) // max(1, len(f"ctunnel-load-{index}-"))))[:args.bytes]
    started = time.perf_counter()
    with socket.create_connection((args.host, args.port), timeout=args.connect_timeout) as sock:
        sock.settimeout(args.io_timeout)
        connected = time.perf_counter()
        if args.slow_write:
            for b in payload:
                sock.send(bytes([b]))
                time.sleep(args.slow_write)
        else:
            sock.sendall(payload)
        if args.half_close:
            sock.shutdown(socket.SHUT_WR)
        if args.slow_read:
            got = bytearray()
            while len(got) < len(payload):
                part = sock.recv(1)
                if not part:
                    raise RuntimeError("connection closed during slow read")
                got.extend(part)
                time.sleep(args.slow_read)
            output = bytes(got)
        else:
            output = recv_exact(sock, len(payload))
        if output != payload:
            raise RuntimeError("payload mismatch")
        if args.hold:
            deadline = time.monotonic() + args.hold
            while time.monotonic() < deadline:
                time.sleep(min(1.0, deadline - time.monotonic()))
    ended = time.perf_counter()
    return connected - started, ended - started


def percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round((pct / 100.0) * (len(ordered) - 1)))))
    return ordered[index]


def main() -> int:
    parser = argparse.ArgumentParser(description="ctunnel TCP load generator")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--concurrency", type=int, default=50)
    parser.add_argument("--bytes", type=parse_size, default=1024)
    parser.add_argument("--rate", type=float, default=0.0, help="new connections per second")
    parser.add_argument("--hold", type=float, default=0.0, help="seconds to keep connection open")
    parser.add_argument("--slow-read", type=float, default=0.0, help="seconds per received byte")
    parser.add_argument("--slow-write", type=float, default=0.0, help="seconds per sent byte")
    parser.add_argument("--half-close", action="store_true")
    parser.add_argument("--connect-timeout", type=float, default=3.0)
    parser.add_argument("--io-timeout", type=float, default=10.0)
    args = parser.parse_args()

    connect_latencies = []
    total_latencies = []
    failures = 0
    lock = threading.Lock()

    def run(index):
        nonlocal failures
        try:
            result = one_connection(args, index)
            with lock:
                connect_latencies.append(result[0])
                total_latencies.append(result[1])
        except Exception as exc:
            with lock:
                failures += 1
            print(f"connection {index} failed: {exc}", file=sys.stderr)

    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = []
        for index in range(args.connections):
            futures.append(executor.submit(run, index))
            if args.rate > 0:
                time.sleep(1.0 / args.rate)
        for future in futures:
            future.result()
    elapsed = time.perf_counter() - started
    success = len(connect_latencies)
    print(f"connections={args.connections} success={success} failures={failures} elapsed={elapsed:.3f}s")
    if connect_latencies:
        print("connect_latency_ms "
              f"p50={percentile(connect_latencies, 50)*1000:.2f} "
              f"p95={percentile(connect_latencies, 95)*1000:.2f} "
              f"p99={percentile(connect_latencies, 99)*1000:.2f} "
              f"avg={statistics.mean(connect_latencies)*1000:.2f}")
        print("total_latency_ms "
              f"p50={percentile(total_latencies, 50)*1000:.2f} "
              f"p95={percentile(total_latencies, 95)*1000:.2f} "
              f"p99={percentile(total_latencies, 99)*1000:.2f} "
              f"avg={statistics.mean(total_latencies)*1000:.2f}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

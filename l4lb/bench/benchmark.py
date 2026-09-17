#!/usr/bin/env python3
"""Reproducible direct-vs-L4LB loopback benchmark; writes raw JSON results."""

import argparse
import concurrent.futures
import json
import math
import os
import signal
import socket
import socketserver
import subprocess
import threading
import time


class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(65536)
            if not data:
                return
            self.request.sendall(data)


class EchoServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1,
                       int(math.ceil(len(ordered) * fraction)) - 1)]


def one_request(port, payload):
    started = time.perf_counter()
    with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        connection.sendall(payload)
        received = bytearray()
        while len(received) < len(payload):
            chunk = connection.recv(min(65536, len(payload) - len(received)))
            if not chunk:
                break
            received.extend(chunk)
    if received != payload:
        raise RuntimeError("payload mismatch")
    return time.perf_counter() - started


def run_case(name, port, requests, concurrency, payload):
    started = time.perf_counter()
    latencies = []
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(one_request, port, payload)
                   for _ in range(requests)]
        for future in concurrent.futures.as_completed(futures):
            try:
                latencies.append(future.result())
            except Exception as error:  # benchmark records all request failures
                errors.append(str(error))
    elapsed = time.perf_counter() - started
    successful_bytes = len(latencies) * len(payload) * 2
    return {
        "name": name,
        "requests": requests,
        "concurrency": concurrency,
        "payload_bytes": len(payload),
        "elapsed_seconds": elapsed,
        "throughput_mib_per_second": successful_bytes / elapsed / 1024 / 1024,
        "p50_ms": percentile(latencies, 0.50) * 1000 if latencies else None,
        "p99_ms": percentile(latencies, 0.99) * 1000 if latencies else None,
        "error_rate": len(errors) / requests,
        "errors": errors[:10],
    }


def process_usage(pid):
    with open("/proc/{}/stat".format(pid), encoding="ascii") as stat_file:
        fields = stat_file.read().split()
    with open("/proc/{}/status".format(pid), encoding="ascii") as status_file:
        status = status_file.read().splitlines()
    ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    rss_kib = next(int(line.split()[1]) for line in status
                   if line.startswith("VmRSS:"))
    return {"cpu_seconds": (int(fields[13]) + int(fields[14])) / ticks,
            "rss_kib": rss_kib}


def wait_for_port(port, process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("proxy exited during startup")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("proxy port did not open")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--concurrency", type=int, default=32)
    parser.add_argument("--payload-bytes", type=int, default=16384)
    parser.add_argument("--output", default="benchmark-report.json")
    args = parser.parse_args()
    if min(args.requests, args.concurrency, args.payload_bytes) <= 0:
        parser.error("benchmark numeric arguments must be positive")

    backend = EchoServer(("127.0.0.1", 9101), EchoHandler)
    backend_thread = threading.Thread(target=backend.serve_forever, daemon=True)
    backend_thread.start()
    proxy = subprocess.Popen([args.server, args.config], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        wait_for_port(9000, proxy)
        payload = bytes((index * 131 + 17) & 0xff
                        for index in range(args.payload_bytes))
        usage_before = process_usage(proxy.pid)
        direct = run_case("direct", 9101, args.requests, args.concurrency, payload)
        proxied = run_case("proxied", 9000, args.requests, args.concurrency, payload)
        usage_after = process_usage(proxy.pid)
        report = {
            "command": " ".join([args.server, args.config]),
            "parameters": vars(args),
            "environment": {"platform": "Linux loopback",
                            "cpu_count": os.cpu_count()},
            "direct": direct,
            "proxied": proxied,
            "proxy_process": {
                "cpu_seconds_during_run":
                    usage_after["cpu_seconds"] - usage_before["cpu_seconds"],
                "rss_kib_after_run": usage_after["rss_kib"],
            },
        }
        with open(args.output, "w", encoding="utf-8") as output:
            json.dump(report, output, indent=2, sort_keys=True)
            output.write("\n")
        print(json.dumps(report, indent=2, sort_keys=True))
    finally:
        if proxy.poll() is None:
            proxy.send_signal(signal.SIGTERM)
            try:
                proxy.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proxy.kill()
                proxy.wait()
        backend.shutdown()
        backend.server_close()
        backend_thread.join()


if __name__ == "__main__":
    main()

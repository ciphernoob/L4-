#!/usr/bin/env python3
"""Process-level smoke test for the assembled L4 proxy."""

import argparse
import os
import signal
import socket
import socketserver
import subprocess
import threading
import time


class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        first_read = True
        while True:
            data = self.request.recv(65536)
            if not data:
                return
            if first_read and data.startswith(b"GET /through-l4 "):
                body = b"http-backend-ok\n"
                response = (b"HTTP/1.1 200 OK\r\nContent-Length: " +
                            str(len(body)).encode("ascii") +
                            b"\r\nConnection: close\r\n\r\n" + body)
                self.request.sendall(response)
                return
            self.request.sendall(data)
            first_read = False


class EchoServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128


def wait_for_port(port, process, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stdout, _ = process.communicate()
            raise RuntimeError("l4lb exited during startup:\n" + stdout)
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise TimeoutError("port {} did not become ready".format(port))


def receive_all(sock):
    chunks = []
    while True:
        data = sock.recv(65536)
        if not data:
            return b"".join(chunks)
        chunks.append(data)


def admin_request(request):
    with socket.create_connection(("127.0.0.1", 9001), timeout=2) as admin:
        admin.sendall(request)
        return receive_all(admin)


def fd_count(pid):
    return len(os.listdir("/proc/{}/fd".format(pid)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--config", required=True)
    args = parser.parse_args()

    backend = EchoServer(("127.0.0.1", 9101), EchoHandler)
    backend_thread = threading.Thread(target=backend.serve_forever, daemon=True)
    backend_thread.start()
    process = subprocess.Popen(
        [args.server, args.config],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        wait_for_port(9000, process)
        wait_for_port(9001, process)
        time.sleep(0.1)
        baseline_fds = fd_count(process.pid)

        payload = b"real\x00process-flow"
        with socket.create_connection(("127.0.0.1", 9000), timeout=2) as client:
            client.sendall(payload)
            client.shutdown(socket.SHUT_WR)
            received = receive_all(client)
        if received != payload:
            raise AssertionError("data-plane payload mismatch")

        with socket.create_connection(("127.0.0.1", 9000), timeout=2) as client:
            client.sendall(
                b"GET /through-l4 HTTP/1.1\r\nHost: backend\r\nConnection: close\r\n\r\n")
            http_response = receive_all(client)
        if b"200 OK" not in http_response or not http_response.endswith(
                b"http-backend-ok\n"):
            raise AssertionError("HTTP byte stream was not forwarded")

        metrics = admin_request(
            b"GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n")
        if b"200 OK" not in metrics or b"l4lb_sessions_total" not in metrics:
            raise AssertionError("admin metrics response is incomplete")
        health = admin_request(
            b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
        missing = admin_request(
            b"GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n")
        method = admin_request(
            b"POST /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n")
        if b"200 OK" not in health or not health.endswith(b"ok\n"):
            raise AssertionError("health response is invalid")
        if b"404 Not Found" not in missing:
            raise AssertionError("unknown admin path was not rejected")
        if b"405 Method Not Allowed" not in method:
            raise AssertionError("unknown admin method was not rejected")

        time.sleep(0.1)
        final_fds = fd_count(process.pid)
        if final_fds > baseline_fds + 1:
            raise AssertionError("fd count grew from {} to {}".format(
                baseline_fds, final_fds))

        process.send_signal(signal.SIGTERM)
        output, _ = process.communicate(timeout=5)
        if process.returncode != 0:
            raise RuntimeError("l4lb exited with {}:\n{}".format(process.returncode,
                                                                  output))
        print("data-plane-ok {} bytes".format(len(received)))
        print("http-through-data-plane-ok {} bytes".format(len(http_response)))
        print("admin-plane-ok {} bytes".format(len(metrics)))
        print("fd-count-ok {} -> {}".format(baseline_fds, final_fds))
        print(output, end="")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        backend.shutdown()
        backend.server_close()
        backend_thread.join()


if __name__ == "__main__":
    main()

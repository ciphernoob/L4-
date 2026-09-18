#!/usr/bin/env python3
"""Teaching proxy: real process round robin, affinity, half-close and cleanup."""
import argparse
import collections
import concurrent.futures
import os
import select
import signal
import socket
import socketserver
import struct
import subprocess
import threading
import time


class Backend(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(5)
        data = self.request.recv(65536)
        # Test backend protocol only; the proxy never parses these commands.
        if data.startswith(b"GET "):
            while b"\r\n\r\n" not in data:
                chunk = self.request.recv(65536)
                if not chunk:
                    return
                data += chunk
            body = b"http-through-l4\n"
            self.request.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: " +
                                 str(len(body)).encode() +
                                 b"\r\nConnection: close\r\n\r\n" + body)
            return
        if data == b"RESET":
            self.request.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                    struct.pack("ii", 1, 0))
            return
        if data == b"WHO":
            self.request.sendall(str(self.server.server_address[1]).encode())
            data = self.request.recv(65536)
        while data:
            self.request.sendall(data)
            data = self.request.recv(65536)


def connect():
    return socket.create_connection(("127.0.0.1", 9000), timeout=5)


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise AssertionError("unexpected EOF")
        data.extend(chunk)
    return bytes(data)


def read_all(sock):
    chunks = []
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)


def exchange(payload):
    with connect() as client:
        client.sendall(payload)
        client.shutdown(socket.SHUT_WR)
        assert read_all(client) == payload


def wait_fd_baseline(process, baseline):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        count = len(os.listdir("/proc/{}/fd".format(process.pid)))
        if count == baseline:
            return
        time.sleep(0.01)
    raise AssertionError("fd leak: {} -> {}".format(baseline, count))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args = parser.parse_args()
    backends, threads = [], []
    process = None
    try:
        for port in (9101, 9102, 9103):
            backend = Backend(("127.0.0.1", port), Handler)
            backends.append(backend)
            thread = threading.Thread(target=backend.serve_forever, daemon=True)
            thread.start()
            threads.append(thread)
        process = subprocess.Popen([args.server], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        # 读启动输出，不额外连接数据端，避免影响 RR 的确定顺序。
        assert select.select([process.stdout], [], [], 5)[0], "startup timeout"
        startup = process.stdout.readline()
        assert "listening" in startup, startup
        baseline = len(os.listdir("/proc/{}/fd".format(process.pid)))
        assert len(os.listdir("/proc/{}/task".format(process.pid))) == 1

        selected = []
        for index in range(6):
            with connect() as client:
                client.sendall(b"WHO")
                selected.append(int(read_exact(client, 4)))
                # 同一连接多次发送；不能重新调度或拆分到其他后端。
                for part in (b"first\x00message", b"second message"):
                    client.sendall(part)
                    assert read_exact(client, len(part)) == part
                client.shutdown(socket.SHUT_WR)
                assert read_all(client) == b""
        assert selected == [9101, 9102, 9103] * 2, selected
        assert collections.Counter(selected) == {9101: 2, 9102: 2, 9103: 2}
        exchange(bytes(range(256)) * 4096)
        with connect() as client:
            client.sendall(b"GET / HTTP/1.1\r\nHost: backend\r\n\r\n")
            assert read_all(client).endswith(b"http-through-l4\n")
        with connect() as client:
            client.sendall(b"RESET")
            try:
                assert client.recv(1) == b""
            except ConnectionResetError:
                pass

        # 并发短连接关闭可让同一 epoll 批次同时包含两端事件。
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(exchange, [b"parallel\x00" * 100] * 80))
        wait_fd_baseline(process, baseline)

        # 停掉全部后端：失败会话应关闭，代理不退出，也没有自动重试。
        for backend in backends:
            backend.shutdown()
            backend.server_close()
        backends.clear()
        with connect() as client:
            try:
                assert client.recv(1) == b""
            except ConnectionResetError:
                pass
        wait_fd_baseline(process, baseline)
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, (stdout, stderr)
        assert not stderr, stderr
        print("PASS: single thread, RR 2/2/2, affinity, binary/HTTP, half-close, reset, refusal, fd cleanup, SIGTERM")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.communicate()
        for backend in backends:
            backend.shutdown()
            backend.server_close()
        for thread in threads:
            thread.join(timeout=2)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run one teaching echo backend: python3 echo_backend.py 9101."""
import argparse
import socketserver


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        print("backend {} accepted {}".format(
            self.server.server_address[1], self.client_address), flush=True)
        try:
            while True:
                data = self.request.recv(65536)
                if not data:
                    return
                self.request.sendall(data)
        except ConnectionError:
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 128


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int)
    args = parser.parse_args()
    with Server(("127.0.0.1", args.port), Echo) as server:
        print("echo backend listening on 127.0.0.1:{}".format(args.port), flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass

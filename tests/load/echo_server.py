#!/usr/bin/env python3
import argparse
import socketserver


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(65536)
            if not data:
                break
            self.request.sendall(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    with socketserver.ThreadingTCPServer((args.host, args.port), Echo) as server:
        server.daemon_threads = True
        server.serve_forever()


if __name__ == "__main__":
    main()

# slow_upstream.py
import time, argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, fmt, *args):  # quieter
        pass

    def do_GET(self):
        # config via query: /slow?chunks=60&sleep_ms=300
        chunks = 60
        sleep = 0.3
        if "chunks=" in self.path:
            try: chunks = int(self.path.split("chunks=")[1].split("&")[0])
            except: pass
        if "sleep_ms=" in self.path:
            try: sleep = int(self.path.split("sleep_ms=")[1].split("&")[0]) / 1000.0
            except: pass

        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()

            def chunk(b: bytes):
                self.wfile.write(f"{len(b):X}\r\n".encode())
                self.wfile.write(b + b"\r\n")
                self.wfile.flush()

            for i in range(1, chunks + 1):
                chunk(f"chunk-{i}\n".encode())
                time.sleep(sleep)

            self.wfile.write(b"0\r\n\r\n")  # end stream
            self.wfile.flush()
            self.wfile.close_connection = True
        except (BrokenPipeError, ConnectionResetError):
            # client went away; just drop
            pass

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()

    ThreadingHTTPServer.daemon_threads = True
    httpd = ThreadingHTTPServer((args.host, args.port), H)
    print(f"Serving on http://{args.host}:{args.port}")
    httpd.serve_forever()

if __name__ == "__main__":
    main()

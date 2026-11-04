"""A stand-in OpenAI-compatible provider for tests (make remotetest).

Listens on 127.0.0.1 only, checks the bearer key, logs each request body
(one JSON line per request) and streams back "(remote) You said: <last user
message>" as server-sent events."""
import json, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

port_file, log_file, key = sys.argv[1], sys.argv[2], sys.argv[3]

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        with open(log_file, "ab") as f:
            f.write(json.dumps({"path": self.path, "auth_ok": self.headers.get("Authorization") == "Bearer " + key,
                                "body": body.decode("utf-8", "replace")}).encode() + b"\n")
        if self.headers.get("Authorization") != "Bearer " + key:
            self.send_response(401); self.end_headers(); return
        req = json.loads(body)
        last = [m["content"] for m in req["messages"] if m["role"] == "user"][-1]
        last = last.split("\n")[-1]  # the author's words (documents come before them)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for piece in ["(remote) You said: ", last]:
            ev = {"choices": [{"delta": {"content": piece}}]}
            self.wfile.write(b"data: " + json.dumps(ev).encode() + b"\n\n")
            self.wfile.flush()
        self.wfile.write(b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n')

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
open(port_file, "w").write(str(srv.server_address[1]))
srv.serve_forever()

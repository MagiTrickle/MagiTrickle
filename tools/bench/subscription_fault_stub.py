#!/usr/bin/env python3
"""Stub subscription-list HTTP server for the Phase 7 fault-injection
soak (tools/bench/run_c_subscription_fault_soak.sh). Serves several
fixed endpoints, each exercising a different mt_sub_fetch_list/
mt_app_sync_due_subscriptions failure mode so the running daemon hits
all of them repeatedly over the soak's duration, not just once:

  /good   -- a valid list whose content alternates every request (forces
             a real "changed" outcome -- rules replaced, rulesets
             rebuilt, DNS snapshot republished -- on every other fetch).
  /loopA  -- 302 to /loopB; /loopB -- 302 to /loopA (redirect loop,
             MT_ERR_INVAL).
  /404    -- non-2xx status (MT_ERR_PROTO).
  /big    -- body over MT_SUB_FETCH_MAX_BODY_BYTES (MT_ERR_LIMIT).

(Connection-refused is exercised by simply pointing a subscription at an
unused port -- no endpoint needed here.)
"""
import http.server
import sys
import threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18300
BIG_BODY = b"a" * (8 * 1024 * 1024 + 1024)

_lock = threading.Lock()
_good_toggle = [False]


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _redirect(self, path):
        self.send_response(302)
        self.send_header("Location", "http://127.0.0.1:%d%s" % (PORT, path))
        self.end_headers()

    def do_GET(self):
        if self.path == "/good":
            with _lock:
                _good_toggle[0] = not _good_toggle[0]
                toggled = _good_toggle[0]
            body = (b"one.fault-soak.example\ntwo.fault-soak.example"
                    if toggled else b"one.fault-soak.example\nthree.fault-soak.example")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/loopA":
            self._redirect("/loopB")
        elif self.path == "/loopB":
            self._redirect("/loopA")
        elif self.path == "/big":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(BIG_BODY)))
            self.end_headers()
            self.wfile.write(BIG_BODY)
        else:
            self.send_response(404)
            self.end_headers()


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()

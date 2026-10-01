#!/usr/bin/env python3
"""HTTP contract driver for the Go-vs-C differential suite (Phase 6,
task #37). Runs a fixed sequence of requests against ONE backend (either
the real Go daemon or magitrickled-c) over TCP (and, for a couple of
spot checks, the Unix socket) and prints a canonical trace to stdout:
one block per step with the request, status code, and a normalized
(key-order-independent) JSON body. run_http_diff.sh runs this script
twice -- once per backend, same base URL/port on each side -- and diffs
the two traces textually; a byte-identical trace across backends is the
"pass" signal (see migration-plan.md's "same requests, normalized
comparison").

Group/subscription top-level IDs are supplied explicitly in request
bodies and ARE expected to be honored literally by both backends (a
brand-new object's own client-supplied ID is used as-is -- see
GroupFromReq/SubscriptionFromReq), so those are compared byte-for-byte.

Nested rule IDs are a different story: neither backend honors a
client-supplied ID for a rule nested inside a brand-new group/
subscription (there's no baseline to match against -- see decisions.md
D-26/D-28's notes on RuleFromReq's lenient reuse), so both assign a
fresh *random* ID there. This script never pretends otherwise: it never
sends an "id" for a newly-created nested rule, learns the real
(different-per-backend) ID from the very response that introduces it
(before that response is even printed, so the introducing step's trace
line is already redacted too), and both (a) uses that real ID to
address the resource correctly in later requests, and (b) redacts it to
a stable per-slot placeholder in the printed trace, so the two
backends' traces compare as identical despite the underlying random
values genuinely differing.

Error message *text* is deliberately normalized away (any `{"error":
"..."}` body has its message redacted to a placeholder before
comparison): decisions.md D-05 already established that byte-identical
serialization -- and by extension exact wording -- is not a goal;
status codes and shape are what the contract actually promises.
"""
import http.client
import http.server
import json
import socket
import sys
import threading

STUB_SUB_PORT = 18099
STUB_SUB_URL = "http://127.0.0.1:%d/list.txt" % STUB_SUB_PORT


class _StubSubListHandler(http.server.BaseHTTPRequestHandler):
    """Serves a fixed subscription list -- reachable identically from
    both the Go daemon and magitrickled-c, so the Phase 7 sync/rules
    steps below (which actually fetch over the network) produce
    byte-identical traces on both sides."""

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path == "/list.txt":
            body = b"one.example\ntwo.example"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()


def start_stub_sub_server():
    server = http.server.HTTPServer(("127.0.0.1", STUB_SUB_PORT), _StubSubListHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def _redact_live_timestamps(obj):
    """Recursively replaces any non-zero "lastUpdate" value with a stable
    placeholder (see Trace._canonical)."""
    if isinstance(obj, dict):
        out = {}
        for k, v in obj.items():
            if k == "lastUpdate" and isinstance(v, (int, float)) and v != 0:
                out[k] = "<TS>"
            else:
                out[k] = _redact_live_timestamps(v)
        return out
    if isinstance(obj, list):
        return [_redact_live_timestamps(x) for x in obj]
    return obj


def _normalize_host_interfaces(obj):
    """Preserve the response contract without freezing runner device names."""
    if not isinstance(obj, dict) or set(obj.keys()) != {"interfaces"}:
        return obj
    interfaces = obj["interfaces"]
    if (
        not isinstance(interfaces, list)
        or len(interfaces) < 2
        or interfaces[0] != {"id": "blackhole"}
        or not all(isinstance(item, dict) and isinstance(item.get("id"), str) for item in interfaces)
    ):
        return obj
    return {"interfaces": [{"id": "blackhole"}, {"id": "<HOST-INTERFACES>"}]}


class UnixHTTPConnection(http.client.HTTPConnection):
    """Minimal HTTPConnection over an AF_UNIX socket (stdlib has no
    built-in support for this)."""

    def __init__(self, path):
        super().__init__("localhost")
        self._unix_path = path

    def connect(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(self._unix_path)
        self.sock = sock


def request(conn, method, path, body=None):
    headers = {"Content-Type": "application/json"} if body is not None else {}
    conn.request(method, path, body=body, headers=headers)
    resp = conn.getresponse()
    data = resp.read()
    return resp.status, data


class Trace:
    """Accumulates one HTTP contract run's output, redacting learned
    dynamic IDs (and error message text -- see module docstring) to
    stable placeholders before printing."""

    def __init__(self, conn, out):
        self.conn = conn
        self.out = out
        self.n = 0
        self.replacements = {}  # real id string -> "<PLACEHOLDER>"

    def _learn(self, real_id, placeholder):
        if real_id and real_id not in self.replacements:
            self.replacements[real_id] = placeholder

    def _redact(self, text):
        for real, placeholder in self.replacements.items():
            text = text.replace(real, placeholder)
        return text

    def _canonical(self, obj):
        if obj is None:
            return "<empty>"
        if isinstance(obj, dict) and set(obj.keys()) == {"error"}:
            obj = {"error": "<ERR>"}
        # A fetch-backed sync (Phase 7) stamps "lastUpdate" with the live
        # wall-clock time, which genuinely differs between the Go run and
        # the (separately started, minutes-later) C run -- not a real
        # behavioral divergence, so it's redacted like error text above.
        # 0 (never synced) is left alone: that's still meaningful signal
        # (confirms neither backend synced when it shouldn't have).
        obj = _redact_live_timestamps(obj)
        return self._redact(json.dumps(obj, sort_keys=True, separators=(",", ":")))

    def step(self, method, path, body=None, learn=None, normalize=None):
        """learn, if given, is called with the parsed response body (or
        None) and must return an iterable of (real_id, placeholder)
        pairs to redact -- applied BEFORE this step's own trace line is
        printed, so even the response that introduces a dynamic ID comes
        out redacted."""
        self.n += 1
        status, data = request(self.conn, method, path, body)
        try:
            parsed = json.loads(data.decode("utf-8")) if data else None
        except ValueError:
            parsed = None
        if normalize:
            parsed = normalize(parsed)
        if learn:
            for real_id, placeholder in learn(parsed):
                self._learn(real_id, placeholder)

        self.out.write("STEP %d %s %s\n" % (self.n, method, self._redact(path)))
        if body is not None:
            raw = body.encode("utf-8") if isinstance(body, str) else body
            try:
                req_obj = json.loads(raw.decode("utf-8")) if raw else None
            except ValueError:
                req_obj = None
            self.out.write("REQBODY %s\n" % self._canonical(req_obj))
        self.out.write("STATUS %d\n" % status)
        self.out.write("BODY %s\n" % self._canonical(parsed))
        self.out.write("---\n")
        return status, parsed


def run_tcp_sequence(base_host, base_port, out):
    conn = http.client.HTTPConnection(base_host, base_port, timeout=5)
    t = Trace(conn, out)

    t.step("GET", "/api/v1/auth")
    t.step("GET", "/api/v1/groups")

    group_body = json.dumps(
        {
            "id": "aabbccdd",
            "name": "g1",
            "color": "#ABCDEF",
            "interface": "eth0",
            "enable": False,
            "rules": [{"name": "r1", "type": "domain", "rule": "example.com", "enable": True}],
        }
    )
    t.step("POST", "/api/v1/groups", group_body,
          learn=lambda r: [(r["rules"][0]["id"], "<RULE-R1>")])

    t.step("GET", "/api/v1/groups?with_rules=true")
    t.step("GET", "/api/v1/groups/aabbccdd?with_rules=true")
    t.step("GET", "/api/v1/groups/nothex!")
    t.step("GET", "/api/v1/groups/deadbeef")

    put_group_body = json.dumps(
        {"name": "g1-renamed", "color": "#222222", "interface": "eth1", "enable": False}
    )
    t.step("PUT", "/api/v1/groups/aabbccdd", put_group_body)

    new_rule_body = json.dumps({"name": "r2", "type": "wildcard", "rule": "*.example.org", "enable": True})
    rule_r2_holder = []

    def learn_rule_r2(resp):
        rule_r2_holder.append(resp["id"])
        return [(resp["id"], "<RULE-R2>")]

    t.step("POST", "/api/v1/groups/aabbccdd/rules", new_rule_body, learn=learn_rule_r2)
    rule_r2 = rule_r2_holder[0]

    t.step("GET", "/api/v1/groups/aabbccdd/rules")
    t.step("GET", "/api/v1/groups/aabbccdd/rules/" + rule_r2)

    put_rule_body = json.dumps(
        {"name": "r2-renamed", "type": "wildcard", "rule": "*.example.net", "enable": False}
    )
    t.step("PUT", "/api/v1/groups/aabbccdd/rules/" + rule_r2, put_rule_body)
    t.step("DELETE", "/api/v1/groups/aabbccdd/rules/" + rule_r2)
    t.step("GET", "/api/v1/groups/aabbccdd/rules")
    t.step("GET", "/api/v1/groups/aabbccdd/rules/deadbeef")

    t.step("DELETE", "/api/v1/groups/aabbccdd")
    t.step("GET", "/api/v1/groups")

    bulk_body = json.dumps(
        {
            "groups": [
                {
                    "id": "10101010",
                    "name": "bulk-1",
                    "color": "#ffffff",
                    "interface": "eth0",
                    "enable": False,
                    "rules": [{"name": "br1", "type": "domain", "rule": "bulk.example.com", "enable": True}],
                },
                {"id": "30303030", "name": "bulk-2", "color": "#ffffff", "interface": "eth1", "enable": False},
            ]
        }
    )
    t.step("PUT", "/api/v1/groups", bulk_body,
          learn=lambda r: [(r["groups"][0]["rules"][0]["id"], "<RULE-BR1>")])
    t.step("GET", "/api/v1/groups?with_rules=true")
    t.step("PUT", "/api/v1/groups", "{}")

    t.step("GET", "/api/v1/system/interfaces", normalize=_normalize_host_interfaces)
    t.step("POST", "/api/v1/system/hooks/netfilterd", json.dumps({"type": "iptables", "table": "nat"}))
    t.step("POST", "/api/v1/system/config/save")

    t.step("GET", "/api/v1/subscriptions")
    # enable:false -- avoids a real-netfilter divergence unrelated to what
    # this suite verifies: both backends now build a real netfilter-
    # backed subscription ruleset when enable=true (Phase 7, decisions.md
    # D-32), which would attempt a real ipset-to-link against "eth0" (not
    # guaranteed to exist identically in every CI sandbox). With
    # enable:false neither side attempts real netfilter work, so this
    # suite stays focused on the config-mutation and fetch-backed-sync
    # contract it's actually meant to verify.
    sub_body = json.dumps(
        {"id": "40404040", "name": "s1", "interface": "eth0", "url": "https://example.com/list.txt", "enable": False}
    )
    t.step("POST", "/api/v1/subscriptions", sub_body)
    t.step("POST", "/api/v1/subscriptions", sub_body)  # duplicate id -> 409
    t.step("POST", "/api/v1/subscriptions", json.dumps({"name": "no-url"}))  # missing url -> 400
    t.step("GET", "/api/v1/subscriptions")

    put_sub_body = json.dumps(
        {
            "subscriptions": [
                {"id": "40404040", "name": "s1-renamed", "url": "https://example.com/list.txt", "enable": False}
            ]
        }
    )
    t.step("PUT", "/api/v1/subscriptions", put_sub_body)
    t.step("GET", "/api/v1/subscriptions")
    t.step("PUT", "/api/v1/subscriptions", "{}")  # missing key -> 400

    # Phase 7: fetch-backed sync/rules-preview endpoints, against a real
    # local stub subscription-list server reachable identically by both
    # backends (see start_stub_sub_server above).
    def learn_preview_rule_ids(resp):
        if not resp:
            return []
        return [(r["id"], "<PREVIEWRULE-%d>" % i) for i, r in enumerate(resp.get("rules") or [])]

    t.step("GET", "/api/v1/subscriptions/rules")  # missing url -> 400
    t.step("GET", "/api/v1/subscriptions/rules?url=" + STUB_SUB_URL, learn=learn_preview_rule_ids)
    t.step("GET", "/api/v1/subscriptions/rules?url=http://127.0.0.1:1/nope")  # connection refused -> 502

    def learn_sync_rule_ids(resp):
        if not resp:
            return []
        return [(r["id"], "<SUBRULE-%d>" % i) for i, r in enumerate(resp.get("rules") or [])]

    t.step("POST", "/api/v1/subscriptions/deadbeef/sync")  # unknown id -> 404
    t.step("POST", "/api/v1/subscriptions/40404040/sync", json.dumps({"url": STUB_SUB_URL}),
          learn=learn_sync_rule_ids)
    t.step("GET", "/api/v1/subscriptions")
    t.step("POST", "/api/v1/subscriptions/40404040/sync")  # re-sync, same content -> unchanged

    t.step("DELETE", "/api/v1/subscriptions/40404040")
    t.step("DELETE", "/api/v1/subscriptions/40404040")  # already gone -> 404
    t.step("GET", "/api/v1/subscriptions")

    conn.close()


def run_unix_spotcheck(unix_path, out):
    conn = UnixHTTPConnection(unix_path)
    t = Trace(conn, out)
    t.step("GET", "/api/v1/groups")
    t.step("GET", "/api/v1/subscriptions")
    t.step("GET", "/api/v1/auth")
    conn.close()


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: contract.py <host> <port> [unix_socket_path]\n")
        return 2
    host, port = sys.argv[1], int(sys.argv[2])
    stub = start_stub_sub_server()
    try:
        run_tcp_sequence(host, port, sys.stdout)
        if len(sys.argv) > 3:
            run_unix_spotcheck(sys.argv[3], sys.stdout)
    finally:
        stub.shutdown()
        stub.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

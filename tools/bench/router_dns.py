#!/usr/bin/env python3
"""Paired on-router measurements driven over SSH, using Python stdlib and dnsload.

Router preparation and limitations are documented in router-benchmark.md.
Never installs packages or stops the production daemon. The prepared chroot
must contain both package binaries; only its private config is overwritten.
"""
import argparse
import hashlib
import json
import shlex
import signal
import subprocess
import time
from pathlib import Path


def config(names, rules, upstream_port):
    # A private namespace prevents the cleaners from touching production rules.
    # Enabled groups exercise matching and real ipset writes. Disabling both
    # iptables families leaves these private sets detached from routing.
    header = f'''configVersion: 0.8.2
app:
  httpWeb:
    enabled: true
    auth:
      enabled: false
    host:
      address: 127.0.0.1
      port: 18080
    skin: default
  dnsProxy:
    host:
      address: 0.0.0.0
      port: 13553
    upstream:
      address: 127.0.0.1
      port: {upstream_port}
    disableRemap53: true
    disableFakePTR: false
    disableDropAAAA: false
    maxIdleConns: 10
    maxConcurrent: 100
    timeout: 5s
  netfilter:
    iptables:
      chainPrefix: MTB_
    ipset:
      tablePrefix: mtb_
      additionalTTL: 1h0m0s
    disableIPv4: true
    disableIPv6: true
    startMarkTableIndex: 1400000000
  link: []
  showAllInterfaces: false
  logLevel: error
groups:
  - id: aa000001
    name: Bench
    color: '#ffffff'
    interface: blackhole
    enable: true
    rules:
'''
    if rules < len(names):
        raise ValueError("rules must cover corpus")
    # Spread corpus matches over the rule list instead of giving Go only early hits.
    positions = {i * (rules - 1) // (len(names) - 1): n for i, n in enumerate(names)}
    body = "".join(f"      - id: {0x10000000+i:08x}\n        name: r{i}\n"
                   f"        type: namespace\n        rule: '{positions.get(i, f'd{i:06d}.bench.invalid')}'\n"
                   "        enable: true\n" for i in range(rules))
    return header + body + "subscriptions: []\n"


def live_rules_config(path, names, upstream_port):
    """Use actual enabled groups and cached subscriptions without routing/fetch.

    Optional PyYAML dependency; the ordinary generated matrix is stdlib-only.
    Never copies app credentials, links, URLs or interface profiles. Rule
    values remain private: this config is sent to the router, not saved to
    the results directory.
    """
    import yaml

    source = yaml.safe_load(path.read_text())
    target = yaml.safe_load(config(names, len(names), upstream_port))
    rule_serial = 0

    def rules(items):
        nonlocal rule_serial
        output = []
        for r in items:
            output.append(dict(id=f"{0x10000000+rule_serial:08x}", name="", type=r["type"],
                               rule=r["rule"], enable=bool(r.get("enable"))))
            rule_serial += 1
        return output

    target["groups"] = [dict(id=f"{0xab000000+i:08x}", name=f"g{i}", color="#ffffff",
                             interface="blackhole", enable=True, rules=rules(g.get("rules", [])))
                        for i, g in enumerate(source.get("groups", [])) if g.get("enable")]
    target["subscriptions"] = [dict(id=f"{0xac000000+i:08x}", name=f"s{i}", interface="blackhole",
                                    enable=True, url="", interval=0, last_update=0,
                                    rules=rules(s.get("rules", [])))
                               for i, s in enumerate(source.get("subscriptions", []))
                               if s.get("enable") and s.get("interface")]
    all_rules = [r for g in target["groups"] + target["subscriptions"] for r in g["rules"]]
    from collections import Counter

    shape = dict(groups=len(target["groups"]), subscriptions=len(target["subscriptions"]),
                 total_rules=len(all_rules), enabled_rules=sum(r["enable"] for r in all_rules),
                 rule_types=dict(Counter(r["type"] for r in all_rules if r["enable"])),
                 source_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    return yaml.safe_dump(target, sort_keys=False), shape


class Router:
    def __init__(self, args):
        self.args = args
        self.base = args.remote_dir
        self.pid = None

    def ssh(self, command, data=None):
        cmd = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-p", str(self.args.ssh_port)]
        if self.args.control_path:
            cmd += ["-S", self.args.control_path]
        return subprocess.check_output(cmd + [self.args.ssh, command], input=data, text=True)

    def snapshot_command(self):
        return (f"cat /proc/{self.pid}/stat; cat /proc/uptime; "
                       f"cat /proc/{self.pid}/status; "
                       f"if [ -r /proc/{self.pid}/smaps ]; then "
                       f"awk '/^Pss:/{{s+=$2}}END{{print \"Pss_kb:\",s}}' /proc/{self.pid}/smaps; fi")
    @staticmethod
    def parse_snapshot(out):
        lines = out.splitlines()
        stat = lines[0].split(")", 1)[1].split()
        result = dict(cpu_ticks=int(stat[11]) + int(stat[12]), uptime=float(lines[1].split()[0]))
        for line in lines[2:]:
            if ":" in line:
                key, value = line.split(":", 1)
                if key in ("VmRSS", "VmHWM", "VmSwap", "Threads", "Pss_kb"):
                    result[key] = int(value.split()[0])
        return result

    def snapshot(self):
        return self.parse_snapshot(self.ssh(self.snapshot_command()))

    def stop(self):
        if self.pid is not None:
            self.ssh(f"kill -TERM {self.pid}; "
                     f"for i in 1 2 3 4 5 6 7 8 9 10; do "
                     f"[ -d /proc/{self.pid} ] || exit 0; sleep 1; done; exit 1")
            self.pid = None

    def start(self, backend, cfg):
        self.stop()
        qbase = shlex.quote(self.base)
        cfgpath = shlex.quote(self.base + "/root/opt/var/lib/magitrickle/config.yaml")
        self.ssh(f"cat > {cfgpath}", cfg)
        command = f"PATH=/opt/bin:/opt/sbin:/bin:/sbin /opt/sbin/chroot {qbase}/root /opt/bin/{backend}"
        if self.args.memory_guard:
            # Only the test child gets the maximum OOM preference. No system
            # sysctls or production process scores are changed.
            guarded = (f"set -e; echo 1000 > /proc/$$/oom_score_adj; "
                       f"export PATH=/opt/bin:/opt/sbin:/bin:/sbin; "
                       f"exec /opt/sbin/chroot {qbase}/root /opt/bin/{backend}")
            command = f"/opt/bin/sh -c {shlex.quote(guarded)}"
        self.pid = int(self.ssh(f"{command} >{qbase}/{backend}.log 2>&1 </dev/null & echo $!"))
        if self.args.memory_guard:
            # Emergency guard belongs to this PID and exits when it exits.
            script = (f"while [ -d /proc/{self.pid} ]; do "
                      "a=$(awk '/MemAvailable:/{print $2}' /proc/meminfo); "
                      "s=$(awk '/SwapFree:/{print $2}' /proc/meminfo); "
                      "if [ \"$a\" -lt 16384 ] || [ \"$s\" -lt 65536 ]; then "
                      f"echo memory_guard > {qbase}/guard-triggered; kill -KILL {self.pid}; exit 0; fi; sleep 1; done")
            self.ssh(f"rm -f {qbase}/guard-triggered; /opt/bin/sh -c {shlex.quote(script)} >/dev/null 2>&1 </dev/null &")

    def load_command(self, proto, duration, rate=0, mode="load", port=13553):
        probe_limit = " -probe-limit 180s" if mode == "probe" and self.args.live_rules else ""
        return (f"{shlex.quote(self.base)}/dnsload -server 127.0.0.1:{port} "
                   f"-corpus {shlex.quote(self.base)}/domains.txt -proto {proto} "
                   f"-concurrency {self.args.concurrency} -duration {duration}s -qps {rate} -mode {mode}{probe_limit}")

    def load(self, proto, duration, rate=0, mode="load", port=13553):
        output = self.ssh(self.load_command(proto, duration, rate, mode, port))
        return json.loads(output) if mode == "load" else float(output)

    def measured_load(self, proto, duration, rate=0):
        # Both snapshots and the load run execute inside one remote command.
        # SSH RTT does not enter the CPU interval.
        snap = self.snapshot_command()
        command = (snap + "; echo __LOAD__; " + self.load_command(proto, duration, rate)
                   + "; echo __AFTER__; " + snap)
        before, rest = self.ssh(command).split("__LOAD__\n", 1)
        output, after = rest.split("__AFTER__\n", 1)
        row = json.loads(output)
        before, after = self.parse_snapshot(before), self.parse_snapshot(after)
        row.update(before=before, after=after, cpu_pct=self.cpu_pct(before, after),
                   cpu_interval_sec=after["uptime"]-before["uptime"])
        return row

    def settle(self):
        # The original daemon can answer DNS before cached subnet syncing
        # completes. Require three quiet intervals before steady-state tests.
        deadline = time.monotonic() + 180
        quiet = 0
        samples = []
        while time.monotonic() < deadline:
            before = self.snapshot()
            time.sleep(5)
            after = self.snapshot()
            cpu = self.cpu_pct(before, after)
            samples.append(cpu)
            quiet = quiet + 1 if cpu < 10 else 0
            if quiet == 3:
                return samples
        raise RuntimeError("daemon did not settle after loading cached rules")

    def cpu_pct(self, before, after):
        elapsed = after["uptime"] - before["uptime"]
        return (after["cpu_ticks"] - before["cpu_ticks"]) / self.args.clk_tck / elapsed * 100


def bench(args):
    if args.repeats < 1 or args.concurrency < 1 or args.duration <= 0 or args.paced_duration <= 0 or args.clk_tck <= 0:
        raise ValueError("repeats, concurrency, duration and CLK_TCK must be positive")
    names = [n.strip() for n in args.corpus.read_text().splitlines() if n.strip() and not n.startswith("#")]
    router = Router(args)
    router.ssh(f"cat > {shlex.quote(router.base)}/domains.txt", args.corpus.read_text())
    args.output.mkdir(parents=True, exist_ok=True)
    # Refuse to append measurements to an existing run accidentally.
    with (args.output / "results.jsonl").open("x") as results:
        def record(row):
            results.write(json.dumps(row, sort_keys=True) + "\n")
            results.flush()

        try:
            if args.live_rules:
                cfg, shape = live_rules_config(args.live_rules, names, args.upstream_port)
                record(dict(metric="live_rules_shape", **shape))
                cases = [(shape["total_rules"], cfg, [("live_rules_paced", "udp", args.paced_duration, 20)])]
            else:
                for proto in (() if args.skip_upstream else ("udp", "tcp")):
                    for rep in range(args.repeats):
                        print(f"upstream {proto=} {rep=}", flush=True)
                        row = router.load(proto, args.duration, port=args.upstream_port)
                        record(dict(metric="upstream", rep=rep, **row))
                cases = []
                for rules in args.rules:
                    cfg = config(names, rules, args.upstream_port)
                    (args.output / f"config-{rules}.yaml").write_text(cfg)
                    cases.append((rules, cfg, [
                        ("capacity", "udp", args.duration, 0),
                        *([] if args.udp_only else [("capacity", "tcp", args.duration, 0)]),
                        *(([("paced", "udp", args.paced_duration, 20)] if rules == 1000 else []))]))
            for rules, cfg, scenarios in cases:
                for scenario, proto, duration, rate in scenarios:
                    for rep in range(args.repeats):
                        order = ["original", "mt-c"] if rep % 2 == 0 else ["mt-c", "original"]
                        order = [backend for backend in order if backend in args.backends]
                        for backend in order:
                            print(f"{rules=} {scenario=} {proto=} {rep=} {backend=}", flush=True)
                            try:
                                router.start(backend, cfg)
                                startup_ms = router.load("udp", 1, mode="probe")
                                record(dict(metric="readiness", backend=backend, rules=rules,
                                            scenario=scenario, proto=proto, rep=rep, probe_ms=startup_ms))
                                if args.live_rules:
                                    samples = router.settle()
                                    record(dict(metric="settle", backend=backend, rules=rules,
                                                scenario=scenario, proto=proto, rep=rep, cpu_samples=samples))
                                before = router.snapshot()
                                time.sleep(5)
                                idle = router.snapshot()
                                record(dict(metric="idle", backend=backend, rules=rules, scenario=scenario,
                                            proto=proto, rep=rep, snapshot=idle,
                                            cpu_pct=router.cpu_pct(before, idle)))
                                warm = router.load(proto, 3)
                                record(dict(metric="warmup", backend=backend, rules=rules, scenario=scenario,
                                            rep=rep, **warm))
                                row = router.measured_load(proto, duration, rate)
                                row.update(metric="load", backend=backend, rules=rules, scenario=scenario,
                                           rep=rep, config_sha256=hashlib.sha256(cfg.encode()).hexdigest())
                                record(row)
                                router.stop()
                            except (subprocess.CalledProcessError, RuntimeError, ValueError, IndexError) as exc:
                                if not args.memory_guard:
                                    raise
                                guard = router.ssh(f"cat {shlex.quote(router.base)}/guard-triggered 2>/dev/null || true").strip()
                                record(dict(metric="failure", backend=backend, rules=rules, scenario=scenario,
                                            proto=proto, rep=rep, reason=guard or "startup_or_measurement",
                                            config_sha256=hashlib.sha256(cfg.encode()).hexdigest(),
                                            exception=type(exc).__name__))
                                router.stop()

        finally:
            router.stop()


def main():
    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssh", default="root@172.16.3.3")
    parser.add_argument("--ssh-port", type=int, default=222)
    parser.add_argument("--control-path")
    parser.add_argument("--remote-dir", default="/tmp/mt-router-bench")
    parser.add_argument("--clk-tck", type=int, required=True, help="router AT_CLKTCK from /proc/self/auxv")
    parser.add_argument("--upstream-port", type=int, default=53)
    parser.add_argument("--corpus", type=Path, default=Path(__file__).with_name("router-domains.txt"))
    parser.add_argument("--live-rules", type=Path, help="private production YAML: actual rules and cached subscriptions; requires PyYAML")
    parser.add_argument("--udp-only", action="store_true", help="only UDP capacity points")
    parser.add_argument("--skip-upstream", action="store_true", help="reuse separately captured upstream baseline")
    parser.add_argument("--memory-guard", action="store_true", help="test child OOM preference and emergency memory floor")
    parser.add_argument("--backends", nargs="+", choices=["original", "mt-c"], default=["original", "mt-c"])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rules", type=int, nargs="+", default=[100, 1000, 10000])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--duration", type=float, default=20)
    parser.add_argument("--paced-duration", type=float, default=30)
    parser.add_argument("--concurrency", type=int, default=10)
    bench(parser.parse_args())


if __name__ == "__main__":
    main()

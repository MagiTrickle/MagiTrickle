#!/usr/bin/env python3
"""SDK workflow contracts; git/make/network are mocked, tar/checksums are real.

This suite checks orchestration, not compilation of an actual OpenWrt SDK.
Run from any directory with: python3 tools/ci/tests/test_openwrt_sdk.py
"""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[3]
ARCH = "aarch64_cortex-a53"
SHA = "a" * 40
STUB = r'''#!/usr/bin/env python3
import json, os, pathlib, sys, tarfile, io
cmd = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
env = os.environ
state = pathlib.Path(env["MOCK_STATE"])
with (state / "calls").open("a") as f:
    f.write(json.dumps([cmd, args]) + "\n")
def stop(message):
    print(message, file=sys.stderr)
    sys.exit(1)
if cmd == "id":
    print(env.get("MOCK_UID", "1001"))
elif cmd in ("sleep", "sudo"):
    pass
elif cmd == "git":
    if "init" in args:
        src = pathlib.Path(args[-1]); src.mkdir()
        target = src / "target/linux/bcm27xx/bcm2710"
        target.mkdir(parents=True)
        (target / "target.mk").write_text("# fixture\n")
        (target.parent / "Makefile").write_text("# fixture\n")
    elif "fetch" in args:
        if env.get("MOCK_FETCH_FAIL"): stop("simulated fetch error")
    elif "rev-parse" in args:
        print(env.get("MOCK_HEAD", "a" * 40))
    elif not any(x in args for x in ("remote", "checkout", "update-ref")):
        stop("unexpected git command")
elif cmd == "make":
    if any(k.startswith("PKG_") for k in env): stop("mt-c PKG_* leaked into OpenWrt")
    stages = [x for x in args if x in ("defconfig", "tools/install", "toolchain/install", "target/linux/compile", "target/sdk/compile", "package/system/apk/host/compile", "package/system/opkg/host/compile")]
    if len(stages) != 1: stop("unexpected make target")
    stage = stages[0]
    if env.get("MOCK_MAKE_FAIL") == stage: stop("simulated source build failure")
    if stage == "defconfig":
        p = pathlib.Path(".config"); s = p.read_text()
        if env.get("MOCK_IGNORE_TARGET"):
            s = s.replace("CONFIG_TARGET_bcm27xx_bcm2710=y", "CONFIG_TARGET_other=y")
        s += 'CONFIG_TARGET_ARCH_PACKAGES="' + env.get("MOCK_ARCH", "aarch64_cortex-a53") + '"\n'
        s += 'CONFIG_LIBC="' + env.get("MOCK_LIBC", "musl") + '"\n'
        if not env.get("MOCK_USE_OPKG"): s += 'CONFIG_USE_APK=y\n'
        p.write_text(s)
    elif stage.endswith("/host/compile"):
        if not env.get("MOCK_NO_HOST_TOOL"):
            manager = "apk" if "/apk/" in stage else "opkg"
            host = pathlib.Path("staging_dir/host/bin"); host.mkdir(parents=True, exist_ok=True)
            tool = host / manager
            tool.write_text("#!/bin/sh\nexit 0\n")
            tool.chmod(0o755)
    elif stage == "target/sdk/compile" and not env.get("MOCK_NO_ARCHIVE"):
        out = pathlib.Path("bin/targets/bcm27xx/bcm2710"); out.mkdir(parents=True)
        with tarfile.open(out / "openwrt-sdk-source.tar.xz", "w:xz") as tar:
            files = {
                "openwrt-sdk-source/Makefile": b"# fixture\n",
                "openwrt-sdk-source/staging_dir/target-aarch64_cortex-a53_musl/include/.keep": b"",
            }
            for tool in ("apk", "opkg"):
                if pathlib.Path("staging_dir/host/bin", tool).is_file():
                    files["openwrt-sdk-source/staging_dir/host/bin/" + tool] = b"#!/bin/sh\nexit 0\n"
            for path, data in files.items():
                entry = tarfile.TarInfo(path); entry.size = len(data)
                entry.mode = 0o755 if "/host/bin/" in path else 0o644
                tar.addfile(entry, io.BytesIO(data))
elif cmd == "curl":
    dest = pathlib.Path(args[args.index("-o") + 1])
    url = next(x for x in args if x.startswith("https://"))
    if url.endswith("version.buildinfo"):
        if env.get("MOCK_REVISION_FAIL"): stop("snapshot metadata unavailable")
        dest.write_text(env.get("MOCK_REVISION", "r123-aaaaaaaaaa"))
    elif "/commits/" in url:
        dest.write_text(json.dumps({"sha": env.get("MOCK_RESOLVED", "a" * 40)}))
    elif url.endswith("sha256sums"):
        dest.write_bytes((state / "manifest").read_bytes())
        if "-w" in args: print(env.get("MOCK_HTTP", "200"), end="")
    else:
        payload = (state / "archive").read_bytes()
        mode = env.get("MOCK_DOWNLOAD", "valid")
        if mode == "fail": stop("simulated archive download failure")
        if mode == "bad":
            dest.write_bytes(b"wrong checksum"); sys.exit(0)
        if mode == "invalid-tar":
            dest.write_bytes(payload); sys.exit(0)
        count = state / "curl-count"
        n = int(count.read_text()) if count.exists() else 0
        count.write_text(str(n + 1))
        if mode == "resume" and n < 2:
            saved = dest.stat().st_size
            if n and saved == 0: stop("partial data was discarded")
            dest.write_bytes(payload[:max(1, len(payload) * (n + 1) // 3)])
            sys.exit(18)
        if mode == "no-range" and n == 1:
            if dest.stat().st_size == 0: stop("expected resume attempt")
            sys.exit(33)
        if mode == "no-range" and n == 0:
            dest.write_bytes(payload[:len(payload)//2]); sys.exit(18)
        if mode == "no-range" and n == 2 and dest.stat().st_size:
            stop("non-resumable partial file was not reset")
        dest.write_bytes(payload)
else:
    stop("unexpected stub command")
'''


class SdkTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="sdk-test-")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.sdk = self.work / "sdk"
        self.logs = self.work / "logs"
        self.bin = self.work / "bin"
        for p in (self.sdk, self.logs, self.bin):
            p.mkdir()
        stub = self.bin / "stub"
        stub.write_text(STUB)
        stub.chmod(0o755)
        for name in ("curl", "git", "make", "sudo", "sleep", "id"):
            (self.bin / name).symlink_to(stub)
        self.env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
                    "MOCK_STATE": str(self.work), "PKG_VERSION": "must-not-leak", "PKG_RELEASE": "bad"}
        self.write_archive()

    def write_archive(self, invalid=False, include_host_tool=True):
        payload = self.work / "archive"
        if invalid:
            payload.write_bytes(b"not a tar archive")
        else:
            with tarfile.open(payload, "w:xz") as tar:
                files = {
                    "openwrt-sdk-fixture/Makefile": b"# fixture\n",
                    "openwrt-sdk-fixture/staging_dir/target-aarch64_cortex-a53_musl/include/.keep": b"",
                }
                if include_host_tool:
                    files["openwrt-sdk-fixture/staging_dir/host/bin/apk"] = b"#!/bin/sh\nexit 0\n"
                for name, data in files.items():
                    entry = tarfile.TarInfo(name); entry.size = len(data)
                    entry.mode = 0o755 if "/host/bin/" in name else 0o644
                    tar.addfile(entry, io.BytesIO(data))
        manifest = hashlib.sha256(payload.read_bytes()).hexdigest() + "  openwrt-sdk-fixture.tar.xz\n"
        (self.sdk / "sha256sums").write_text(manifest)
        (self.work / "manifest").write_text(manifest)

    def run_script(self, name, *args, expect=0, **env):
        p = subprocess.run(["bash", str(ROOT / "tools/ci" / name), *map(str, args)],
                           cwd=ROOT, env={**self.env, **env}, text=True, capture_output=True, timeout=20)
        if expect == 0:
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
        return p

    def build(self, version="25.12.5", expect=0, **env):
        env.setdefault("MOCK_USE_OPKG", "1" if version.startswith("24.") else "")
        return self.run_script("build-openwrt-sdk.sh", version, "bcm27xx", "bcm2710", ARCH,
                               self.sdk, self.logs, expect=expect, **env)

    def download(self, expect=0, **env):
        return self.run_script("download-openwrt-sdk.sh", self.sdk, "https://fixture.invalid/sdk", self.logs,
                               expect=expect, **env)

    def calls(self, name):
        p = self.work / "calls"
        return [args for cmd, args in map(json.loads, p.read_text().splitlines()) if cmd == name] if p.exists() else []

    def test_valid_download_does_not_build(self):
        self.download()
        self.assertTrue((self.sdk / "openwrt-sdk-fixture/Makefile").exists())
        self.assertFalse(self.calls("git"))

    def test_resume_after_two_interruptions(self):
        self.download(MOCK_DOWNLOAD="resume")
        self.assertEqual(len(self.calls("curl")), 3)

    def test_range_rejection_restarts(self):
        self.download(MOCK_DOWNLOAD="no-range")
        self.assertEqual(len(self.calls("curl")), 3)

    def test_bad_hash_is_not_extracted(self):
        self.download(expect=1, MOCK_DOWNLOAD="bad")
        self.assertFalse((self.sdk / "openwrt-sdk-fixture").exists())
        self.assertEqual(len(self.calls("curl")), 6)

    def test_bad_archive_leaves_no_partial_sdk(self):
        self.write_archive(invalid=True)
        self.download(expect=1)
        self.assertFalse(list(self.sdk.glob(".extract.*")))
        self.assertFalse(list(self.sdk.glob("openwrt-sdk-*/")))

    def test_release_is_pinned_and_stages_are_ordered(self):
        self.build()
        self.assertIn("refs/tags/v25.12.5", next(a for a in self.calls("git") if "fetch" in a))
        stages = [next(x for x in a if x == "defconfig" or "/" in x) for a in self.calls("make")]
        self.assertEqual(stages, ["defconfig", "tools/install", "toolchain/install",
                                  "target/linux/compile", "package/system/apk/host/compile",
                                  "target/sdk/compile"])
        self.assertTrue((self.sdk / "openwrt-sdk-source/Makefile").exists())
        self.assertTrue(os.access(self.sdk / "openwrt-sdk-source/staging_dir/host/bin/apk", os.X_OK))
        self.assertIn(SHA, (self.logs / "source-revision.txt").read_text())
        self.assertFalse(list(self.work.glob("sdk.source.*")))

    def test_older_release_keeps_its_tag(self):
        self.build(version="24.10.4")
        self.assertIn("refs/tags/v24.10.4", next(a for a in self.calls("git") if "fetch" in a))
        self.assertTrue(any("package/system/opkg/host/compile" in a for a in self.calls("make")))
        self.assertTrue(os.access(self.sdk / "openwrt-sdk-source/staging_dir/host/bin/opkg", os.X_OK))

    def test_snapshot_uses_resolved_commit(self):
        self.build(version="snapshot")
        self.assertIn(SHA, next(a for a in self.calls("git") if "fetch" in a))

    def test_snapshot_without_revision_fails_closed(self):
        self.build(version="snapshot", expect=1, MOCK_REVISION_FAIL="1")
        self.assertFalse(self.calls("git"))

    def test_snapshot_revision_mismatch_is_rejected(self):
        self.build(version="snapshot", expect=1, MOCK_RESOLVED="b" * 40)
        self.assertFalse(self.calls("git"))

    def test_wrong_arch_rejected_before_toolchain(self):
        self.build(expect=1, MOCK_ARCH="mips_24kc")
        self.assertEqual(len(self.calls("make")), 1)

    def test_non_musl_toolchain_rejected(self):
        self.build(expect=1, MOCK_LIBC="glibc")
        self.assertEqual(len(self.calls("make")), 1)

    def test_ignored_kconfig_target_rejected(self):
        self.build(expect=1, MOCK_IGNORE_TARGET="1")
        self.assertEqual(len(self.calls("make")), 1)

    def test_missing_host_package_manager_fails_before_sdk_packaging(self):
        p = self.build(expect=1, MOCK_NO_HOST_TOOL="1")
        self.assertIn("host package manager apk missing", p.stderr)
        self.assertFalse(any("target/sdk/compile" in a for a in self.calls("make")))

    def test_host_package_manager_compile_failure_propagates(self):
        self.build(expect=1, MOCK_MAKE_FAIL="package/system/apk/host/compile")
        self.assertFalse(any("target/sdk/compile" in a for a in self.calls("make")))

    def test_source_compile_failure_propagates(self):
        self.build(expect=1, MOCK_MAKE_FAIL="toolchain/install")
        self.assertEqual(len(self.calls("make")), 3)
        self.assertFalse(list(self.sdk.glob("openwrt-sdk-*/")))

    def test_missing_built_archive_is_an_error(self):
        self.build(expect=1, MOCK_NO_ARCHIVE="1")

    def test_missing_release_does_not_use_main(self):
        self.build(expect=1, MOCK_FETCH_FAIL="1")
        self.assertFalse(self.calls("make"))
        self.assertEqual(len([a for a in self.calls("git") if "fetch" in a]), 3)

    def test_build_as_root_is_rejected(self):
        self.build(expect=1, MOCK_UID="0")
        self.assertFalse(self.calls("git"))

    def acquisition(self, **extra):
        # Execute the actual acquisition + architecture-check block from the
        # workflow, not a second implementation of its fallback condition.
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        start = workflow.index('          version="${{ steps.toolchain.outputs.openwrt_version }}"')
        end = workflow.index('          pkg_dir="$sdk_dir/package/net/mt-c"', start)
        body = textwrap.dedent(workflow[start:end])
        for key, value in {"steps.toolchain.outputs.openwrt_version": "25.12.5",
                           "steps.toolchain.outputs.openwrt_target": "bcm27xx",
                           "steps.toolchain.outputs.openwrt_subtarget": "bcm2710", "matrix.name": ARCH, "matrix.package_ext": "apk"}.items():
            body = body.replace("${{ " + key + " }}", value)
        p = subprocess.run(["bash", "-eu", "-c", body], cwd=ROOT,
                           env={**self.env, "RUNNER_TEMP": str(self.work), "GITHUB_OUTPUT": str(self.work / "output"),
                                "GITHUB_STEP_SUMMARY": str(self.work / "summary"), **extra},
                           capture_output=True, text=True, timeout=20)
        return p

    def test_workflow_valid_download_skips_source(self):
        p = self.acquisition()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertFalse(self.calls("git"))
        self.assertIn("sdk_origin=prebuilt", (self.work / "output").read_text())

    def test_workflow_rejects_sdk_without_package_manager(self):
        self.write_archive(include_host_tool=False)
        p = self.acquisition()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("lacks the required host package manager", p.stderr)
        self.assertFalse(self.calls("git"))

    def test_workflow_archive_failure_builds_source(self):
        p = self.acquisition(MOCK_DOWNLOAD="fail")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr + "\n" + str(self.calls("make")))
        self.assertIn("sdk_origin=source", (self.work / "output").read_text())
        self.assertTrue(self.calls("git"))

    def test_workflow_manifest_unavailable_builds_source(self):
        p = self.acquisition(MOCK_HTTP="503")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("sdk_origin=source", (self.work / "output").read_text())

    def test_workflow_failed_source_is_not_success(self):
        p = self.acquisition(MOCK_DOWNLOAD="fail", MOCK_MAKE_FAIL="toolchain/install")
        self.assertNotEqual(p.returncode, 0)
        self.assertNotIn("sdk_origin=source", (self.work / "output").read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)

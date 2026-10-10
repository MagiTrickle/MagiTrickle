#!/usr/bin/env python3
"""Offline packaging-contract smoke test, not a cross-build or router install.

Build real IPK archives from an explicitly synthetic payload. For APK, capture
mkpkg's arguments and inspect the staged inventories/hooks (no fake APK is
claimed to be installable). SDK recipes are evaluated with minimal make stubs.
"""
from __future__ import annotations

import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

REPO = Path(__file__).resolve().parents[2]


def run(args: list[str], cwd: Path, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{args!r}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def ipk_contract(work: Path, platform: str, target: str) -> None:
    key = f"{platform}_{target}"
    compile_dir = work / ".build" / key / "compile"
    compile_dir.mkdir(parents=True)
    for name in ("magitrickled", "mt-c-updater"):
        executable = compile_dir / name
        executable.write_text("#!/bin/sh\n# Synthetic payload: never install this test package.\nexit 0\n")
        executable.chmod(0o755)
    variables = [f"PLATFORM={platform}", f"TARGET={target}", "PKG_VERSION=1.2.3", "PKG_REVISION=1"]
    run(["make", "-o", "build", "package_ipk", *variables], work)
    package = work / ".build" / f"mt-c_1.2.3-1_{key}.ipk"
    with tarfile.open(package) as outer:
        def inner(name: str) -> tarfile.TarFile:
            member = outer.extractfile(f"./{name}.tar.gz")
            assert member
            return tarfile.open(fileobj=io.BytesIO(member.read()), mode="r:gz")
        with inner("control") as control:
            member = control.extractfile("./control")
            assert member
            fields = dict(line.split(": ", 1) for line in member.read().decode().splitlines())
            assert fields["Package"] == "mt-c"
            assert fields["Conflicts"] == "magitrickle"
            assert fields["Version"] == "1.2.3-1"
            assert "Replaces" not in fields and "Provides" not in fields
        with inner("data") as payload:
            files = set(payload.getnames())
            binary_dir = "./opt/bin" if platform == "entware" else "./usr/bin"
            config = "./opt/var/lib/magitrickle/config.yaml" if platform == "entware" else "./etc/magitrickle/state/config.yaml"
            assert config in files
            for name in ("magitrickled", "mt-c-updater"):
                binary = f"{binary_dir}/{name}"
                assert binary in files
                assert payload.getmember(binary).mode & 0o111
    print(f"PASS real IPK metadata and preserved payload paths: {key}")


def apk_contract(work: Path) -> None:
    bindir = work / "test-bin"
    bindir.mkdir()
    recorder = bindir / "apk"
    recorder.write_text("#!/usr/bin/env python3\nimport json, os, sys\nfrom pathlib import Path\nPath(os.environ['APK_ARGS']).write_text(json.dumps(sys.argv[1:]))\n")
    recorder.chmod(0o755)
    key = work / "test-only-key"
    key.write_text("not a key; mkpkg is an argument recorder in this test\n")
    args_path = work / "apk-args.json"
    env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ["PATH"], APK_ARGS=str(args_path))
    run(["make", "-o", "build", "package_apk", "ROOT_WRAP=", "PLATFORM=openwrt", "TARGET=x86_64",
         "PKG_VERSION=1.2.3", "PKG_REVISION=1", f"BUILD_KEY_APK_SEC={key}"], work, env)
    args = json.loads(args_path.read_text())
    assert args[0] == "mkpkg"
    info = [args[i + 1] for i, value in enumerate(args[:-1]) if value == "-I"]
    assert "name:mt-c" in info
    depends = next(item for item in info if item.startswith("depends:"))
    assert "!magitrickle" in depends.split()
    assert not any(item.startswith(("provides:", "replaces:")) for item in info)
    root = work / ".build/openwrt_x86_64/root_apk"
    assert (root / "lib/apk/packages/mt-c.list").is_file()
    assert (root / "lib/apk/packages/mt-c.conffiles").is_file()
    inventory = (root / "lib/apk/packages/mt-c.list").read_text().splitlines()
    for name in ("magitrickled", "mt-c-updater"):
        assert f"/usr/bin/{name}" in inventory
        assert (root / "usr/bin" / name).stat().st_mode & 0o111
    assert not list((root / "lib/apk/packages").glob("magitrickle.*"))
    for script in (work / ".build/openwrt_x86_64/apk").glob("*.sh"):
        assert 'pkgname="mt-c"' in script.read_text()
        run(["sh", "-n", str(script)], work)
    print("PASS APK mkpkg conflict argument, package inventories and lifecycle hooks (staging only)")


def sdk_contract(work: Path, platform: str) -> None:
    sdk = work / f"sdk-{platform}"
    sdk.mkdir()
    (sdk / "rules.mk").write_text("INCLUDE_DIR:=$(CURDIR)\nBUILD_DIR:=$(CURDIR)/build\n")
    (sdk / "version.mk").write_text("MAGITRICKLE_VERSION:=1.2.3\nMAGITRICKLE_RELEASE:=1\nMAGITRICKLE_TARGET:=aarch64-3.10_kn\n")
    # Evaluate the actual package definition, rather than a grep of its text.
    (sdk / "package.mk").write_text("define BuildPackage\n$$(eval $$(Package/$(1)))\n.PHONY: check\ncheck:\n\t@echo 'name=$(1)'\n\t@echo 'conflicts=$$(CONFLICTS)'\n\t@echo 'source=$$(PKG_SOURCE)'\nendef\n")
    shutil.copy(REPO / f"tools/ci/{platform}-package/Makefile", sdk / "Makefile")
    output = run(["make", "-s", "check", f"TOPDIR={sdk}"], sdk)
    assert "name=mt-c\n" in output and "conflicts=magitrickle\n" in output
    assert "source=mt-c-source.tar.gz\n" in output
    print(f"PASS evaluated {platform} SDK package identity/conflicts/source name")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="mt-package-test-") as directory:
        work = Path(directory)
        shutil.copy(REPO / "Makefile", work / "Makefile")
        shutil.copytree(REPO / "files", work / "files", symlinks=True)
        dist = work / "src/frontend/dist"
        dist.mkdir(parents=True)
        (dist / "index.html").write_text("<title>synthetic packaging fixture</title>\n")
        ipk_contract(work, "entware", "aarch64-3.10_kn")
        ipk_contract(work, "openwrt", "x86_64")
        apk_contract(work)
        sdk_contract(work, "entware")
        sdk_contract(work, "openwrt")


if __name__ == "__main__":
    main()

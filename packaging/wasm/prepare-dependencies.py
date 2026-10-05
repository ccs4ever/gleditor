#!/usr/bin/env python3
"""Generate a Poppler libc++ overlay from the exact pinned vcpkg port.

Keep dependency metadata and upstream port logic in vcpkg. Only the small
constructor fix is maintained here; no installed or thirdparty file is edited.
"""

import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    source = Path(__file__).parent
    vcpkg = Path(sys.argv[1]).resolve()
    destination = Path(sys.argv[2]).resolve() / "poppler"
    baseline = json.loads((source / "vcpkg.json").read_text())["builtin-baseline"]
    revision = subprocess.check_output(["git", "-C", str(vcpkg), "rev-parse", "HEAD"], text=True).strip()
    if revision != baseline:
        raise SystemExit(f"WebAssembly dependency overlay requires vcpkg {baseline}; found {revision}")
    shutil.copytree(vcpkg / "ports" / "poppler", destination, dirs_exist_ok=True)
    shutil.copyfile(source / "poppler-libcxx.patch", destination / "poppler-libcxx.patch")
    portfile = destination / "portfile.cmake"
    port = portfile.read_text()
    expected = "        private-namespace.patch\n"
    if port.count(expected) != 1:
        raise SystemExit("Pinned Poppler port no longer has the expected patch list")
    portfile.write_text(port.replace(expected, expected + "        poppler-libcxx.patch\n"))


if __name__ == "__main__":
    main()

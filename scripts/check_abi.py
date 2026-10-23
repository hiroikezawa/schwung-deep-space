#!/usr/bin/env python3
"""Reject a deep-space.so the Move cannot load, or one that could collide with a
neighbour: AArch64, glibc <= 2.34, only libc/libm (+ the loader-provided
libgcc_s if the toolchain insists), one exported entry point.

Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
"""
import re
import subprocess
import sys

path = sys.argv[1]
header = subprocess.check_output(["readelf", "-h", path], text=True)
assert "AArch64" in header, "not an AArch64 build"

versions = subprocess.check_output(["readelf", "--version-info", path], text=True)
glibc = {tuple(map(int, s.split("."))) for s in re.findall(r"GLIBC_([0-9.]+)", versions)}
assert glibc and max(glibc) <= (2, 34), f"Move-incompatible GLIBC: {sorted(glibc)}"

dynamic = subprocess.check_output(["readelf", "-d", path], text=True)
needed = re.findall(r"\(NEEDED\).*\[(.+?)\]", dynamic)
allowed = {"libc.so.6", "libm.so.6", "ld-linux-aarch64.so.1"}
assert set(needed) <= allowed, f"unexpected shared libraries: {needed}"

symbols = subprocess.check_output(["readelf", "-W", "--dyn-syms", path], text=True)
exported = []
undefined = []
for line in symbols.splitlines():
    f = line.split()
    # Num: Value Size Type Bind Vis Ndx Name
    if len(f) >= 8 and f[4] in ("GLOBAL", "WEAK"):
        (undefined if f[6] == "UND" else exported).append(f[7])
assert exported == ["move_audio_fx_init_v2"], f"unexpected exports: {exported}"
# Allocation would only ever be reachable by mistake; refuse it outright.
heap = [s for s in undefined if re.match(r"(malloc|calloc|realloc|free|_Zn[wa]|_Zd[la]|pthread_mutex)", s)]
assert not heap, f"heap/lock symbols referenced: {heap}"
print("PASS: AArch64, only move_audio_fx_init_v2 exported, NEEDED", needed,
      "max GLIBC", ".".join(map(str, max(glibc))), "- no malloc/new/mutex imported")

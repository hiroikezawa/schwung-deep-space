#!/usr/bin/env python3
"""Fail if anything copyleft could have crept into the source or the package.

  * every file we wrote carries the Hiro Ikezawa copyright line;
  * no GPL / LGPL / AGPL text or SPDX tag anywhere in what is compiled or
    shipped (the licence documents themselves excepted, which name them only
    to say they are NOT used);
  * the only third-party code is Schwung's plugin API headers, kept with
    their MIT notice;
  * the package carries LICENSE and THIRD_PARTY_LICENSES.md.

Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
"""
import pathlib
import re
import sys
import tarfile

root = pathlib.Path(__file__).resolve().parent.parent
errors = []
ours = [p for p in (root / "dsp").glob("*.[ch]")] + list((root / "tests").glob("*")) + \
       list((root / "scripts").glob("*"))
for f in sorted(ours):
    if f.is_file() and f.suffix in (".c", ".h", ".mjs", ".js", ".py", ".sh"):
        if "Copyright (c) 2026 Hiro Ikezawa" not in f.read_text(errors="replace"):
            errors.append(f"{f.relative_to(root)}: copyright line missing")

copyleft = re.compile(r"GNU (Lesser |Affero )?General Public|SPDX-License-Identifier:\s*\S*GPL|\bL?A?GPL-?[23]")
exempt = {"LICENSE", "THIRD_PARTY_LICENSES.md", "README.md", "check_licenses.py"}
for f in sorted(root.rglob("*")):
    rel = f.relative_to(root)
    if not f.is_file() or rel.parts[0] in ("build", "dist", ".git") or f.name in exempt:
        continue
    if f.suffix not in (".c", ".h", ".mjs", ".js", ".json", ".py", ".sh"):
        continue
    if copyleft.search(f.read_text(errors="replace")):
        errors.append(f"{rel}: copyleft licence text")

api = root / "third_party" / "schwung"
if "Permission is hereby granted, free of charge" not in (api / "LICENSE").read_text():
    errors.append("third_party/schwung/LICENSE: MIT notice missing")
third = sorted(p.relative_to(root) for p in (root / "third_party").rglob("*") if p.is_file())
expected = [pathlib.Path("third_party/schwung/LICENSE"),
            pathlib.Path("third_party/schwung/host/audio_fx_api_v2.h"),
            pathlib.Path("third_party/schwung/host/plugin_api_v1.h")]
if third != expected:
    errors.append(f"unexpected third-party files: {third}")

pkg = root / "dist" / "deep-space-module.tar.gz"
if pkg.exists():
    with tarfile.open(pkg) as t:
        names = set(t.getnames())
    for need in ("deep-space/LICENSE", "deep-space/THIRD_PARTY_LICENSES.md", "deep-space/deep-space.so"):
        if need not in names:
            errors.append(f"package lacks {need}")

if errors:
    print("\n".join("FAIL: " + e for e in errors))
    sys.exit(1)
print(f"PASS: {len(ours)} own files carry the copyright line; no copyleft text; only Schwung's MIT API "
      f"headers vendored; package notices {'present' if pkg.exists() else 'not built yet'}")

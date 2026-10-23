#!/usr/bin/env bash
# Build the Deep Space Convolution module.
#   scripts/build.sh native   desktop deep-space.so + objects for the tests (build/native)
#   scripts/build.sh asan     the same with AddressSanitizer + UBSan (build/asan)
#   scripts/build.sh arm64    Move deep-space.so + install tarball (dist/)
#
# Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
set -euo pipefail
ID=deep-space
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
# Schwung's plugin API headers ship in third_party/schwung (MIT, see its
# LICENSE), unmodified.
API="$MODULE/third_party/schwung"
TARGET="${1:-arm64}"

SAN=()
case "$TARGET" in
  native) CC="${CC:-gcc}"; OUT="$MODULE/build/native" ;;
  asan) CC="${CC:-gcc}"; OUT="$MODULE/build/asan"
        SAN=(-g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
  arm64) CC="${CROSS_PREFIX:-aarch64-linux-gnu-}gcc"; OUT="$MODULE/dist/$ID" ;;
  *) echo "Usage: $0 [native|asan|arm64]" >&2; exit 2 ;;
esac

GEN="$MODULE/build/generated"
OBJ="$MODULE/build/obj-$TARGET"
rm -rf "$OBJ"
# The package directory is rebuilt from nothing: a file dropped from the module
# must not ride along from an earlier build.
[[ "$TARGET" == arm64 ]] && rm -rf "$OUT" "$MODULE/dist/$ID-module.tar.gz"
mkdir -p "$OUT" "$GEN" "$OBJ"
python3 "$MODULE/scripts/metadata.py" "$GEN"

# -fvisibility=hidden + -Bsymbolic: only move_audio_fx_init_v2 is exported and
#   every internal call binds locally.
# No -ffast-math: the NaN/Inf guards must survive. -ffp-contract=off keeps the
#   float arithmetic what the source says on both targets.
CFLAGS=(-std=gnu11 -O3 -fPIC -fno-strict-aliasing -fno-math-errno -ffp-contract=off
        -fvisibility=hidden -Wall -Wextra -Werror -Wshadow
        -I"$MODULE/dsp" -I"$GEN" -I"$API")

objs=()
for src in "$MODULE"/dsp/*.c; do
  obj="$OBJ/$(basename "${src%.c}").o"
  "$CC" "${CFLAGS[@]}" "${SAN[@]}" -c "$src" -o "$obj"
  objs+=("$obj")
done
if [[ "$TARGET" == asan ]]; then   # the sanitizer runtimes come from the test executable
  "$CC" -shared "${SAN[@]}" -o "$OUT/$ID.so" "${objs[@]}" -Wl,-Bsymbolic -lm
else
  "$CC" -shared -o "$OUT/$ID.so" "${objs[@]}" -Wl,-z,defs -Wl,-Bsymbolic -Wl,--gc-sections -lm
fi

cp "$MODULE/module.json" "$OUT/"
if [[ "$TARGET" == arm64 ]]; then
  python3 "$MODULE/scripts/check_abi.py" "$OUT/$ID.so"
  for f in help.json README.md LICENSE THIRD_PARTY_LICENSES.md; do
    cp "$MODULE/$f" "$OUT/"
  done
  tar -czf "$MODULE/dist/$ID-module.tar.gz" -C "$MODULE/dist" "$ID"
fi
file "$OUT/$ID.so"

#!/bin/bash
# Tests for the C++ helper: tests/test_helper.sh
# Builds it, runs its self-test, then checks it against real processes.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$work"' EXIT
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

bin="$work/downloads"
c++ -std=c++17 -O2 -Wall -Wextra -Werror -o "$bin" "$here/downloads.cpp"
"$bin" --self-test >/dev/null || fail "self-test"

# --once prints one valid snapshot.
"$bin" --once --interval 0.5 | jq -e '.v == 1 and (.jobs | type == "array")' >/dev/null || fail "--once snapshot"

# --demo produces jobs with the documented fields.
{ timeout 3 "$bin" --demo --interval 0.5 || true; } | head -1 |
  jq -e '.jobs[0] | has("id","kind","title","detail","state","done","total","rate","approx","files")' >/dev/null ||
  fail "demo fields"

# A process named "curl" that slowly writes a .part file is detected.
ln -s "$(command -v bash)" "$work/curl"
"$work/curl" -c 'for i in $(seq 60); do head -c 65536 /dev/zero; sleep 0.1; done' >"$work/big.bin.part" &
writer=$!
snap=$(timeout 5 "$bin" --interval 0.5 | head -6 | tail -1 || true)
echo "$snap" | jq -e --arg id "pid:$writer" \
  '.jobs | map(select(.id == $id)) | .[0] | .kind == "curl" and .detail == "big.bin" and .rate > 0' >/dev/null ||
  fail "live .part detection: $snap"
kill $writer 2>/dev/null || true

# Steam progress comes from app manifests while a "steam" process runs.
mkdir -p "$work/home/.local/share/Steam/steamapps"
acf="$work/home/.local/share/Steam/steamapps/appmanifest_570.acf"
write_acf() { printf '"AppState"\n{\n"appid" "570"\n"name" "Dota 2"\n"BytesToDownload" "1000000"\n"BytesDownloaded" "%s"\n}\n' "$1" >"$acf"; }
printf '"libraryfolders"\n{\n"0"\n{\n"path" "%s"\n}\n}\n' "$work/home/.local/share/Steam" \
  >"$work/home/.local/share/Steam/steamapps/libraryfolders.vdf"
write_acf 1000
ln -s "$(command -v sleep)" "$work/steam"
"$work/steam" 30 &
( for n in 2000 50000 90000 130000 170000 210000 250000; do sleep 0.5; write_acf $n; done ) &
snap=$(HOME="$work/home" timeout 6 "$bin" --interval 0.5 |
  jq -c --unbuffered 'select(any(.jobs[]; .id == "steam:570"))' | head -3 | tail -1 || true)
echo "$snap" | jq -e '.jobs | map(select(.id == "steam:570")) | .[0] |
  .detail == "Dota 2" and .total == 1000000 and .rate > 0' >/dev/null ||
  fail "steam manifest progress: $snap"

# run.sh builds into the cache and runs the helper.
XDG_CACHE_HOME="$work/cache" sh "$here/run.sh" --self-test >/dev/null || fail "run.sh build"
ls "$work/cache/omarchy-downloads-monitor/"downloads-* >/dev/null 2>&1 || fail "run.sh cache binary"

if [ "$fails" -eq 0 ]; then echo "ok helper tests"; else exit 1; fi

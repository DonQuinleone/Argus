#!/usr/bin/env bash
#
# Regression check: synthesise defect fixtures and assert each detector fires
# (and that a clean file stays clean). Run from the repo root after building.
#
set -u
# NB: no pipefail here - argus intentionally exits non-zero on warn/fail.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARGUS="${ARGUS:-$ROOT/build/argus}"
FX="${FX:-/tmp/argus_fixtures}"

if [[ ! -x "$ARGUS" ]]; then
  echo "error: argus not found at $ARGUS (build first, or set ARGUS=)" >&2
  exit 1
fi

echo "Generating fixtures in $FX ..."
python3 "$ROOT/tests/make_fixtures.py" "$FX" >/dev/null || {
  echo "fixture generation failed (need python3 + numpy)"; exit 1; }

pass=0 fail=0

# assert <file> <grep-pattern> <description>
assert() {
  local file="$FX/$1.wav" pat="$2" desc="$3" out
  out="$("$ARGUS" "$file" 2>/dev/null)"  # argus exit code is informational, ignore it
  if echo "$out" | grep -qE "$pat"; then
    echo "  PASS  $desc"
    pass=$((pass+1))
  else
    echo "  FAIL  $desc  (expected /$pat/ in $1.wav)"
    fail=$((fail+1))
  fi
}

echo "Running assertions ..."
assert clean             "Verdict: INFO"                                  "clean file is clean"
assert dropout           "Dropout.*noise floor vanishes"                  "dropout detected"
assert zerogap           "Digital silence gap"                            "embedded zero-gap detected"
assert clipping          "Clipping.*clipped samples|True-peak overs"      "clipping detected"
assert clicks            "Clicks.*click"                                  "clicks detected"
assert dc_offset         "DC offset present"                              "DC offset detected"
assert dead_channel      "channel is silent"                              "dead channel detected"
assert out_of_phase      "out of phase"                                   "out-of-phase detected"
assert head_tail_silence "Lead-in 0.500"                                  "head/tail silence measured"
assert fake_hires        "Hard cutoff|upsampled|lossy"                    "brickwall/upsample detected"

echo
echo "Results: $pass passed, $fail failed"
[[ $fail -eq 0 ]]

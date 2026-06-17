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
assert clean             "Verdict: PASS"                                  "clean file is clean"
assert dropout           "Dropout.*noise floor vanishes"                  "dropout detected"
assert zerogap           "Digital silence gap"                            "embedded zero-gap detected"
assert clipping          "Clipping.*clipped samples|True-peak overs"      "clipping detected"
assert baked_clipping    "baked-in clipping"                              "baked-in (sub-FS) clipping detected"
assert clicks            "Clicks.*click"                                  "clicks detected"
assert clicks            "Locations:.*0:00:00.500.*0:00:01.200"           "all click timecodes listed"
assert dc_offset         "DC offset present"                              "DC offset detected"
assert dead_channel      "channel is silent"                              "dead channel detected (stereo)"
assert out_of_phase      "out of phase"                                   "out-of-phase detected"
assert head_tail_silence "Lead-in 0.500"                                  "head/tail silence measured"
assert fake_hires        "Hard cutoff|upsampled|lossy"                    "brickwall/upsample detected"
# Multichannel: classified as surround, NOT failed on channel count / dead channel.
assert surround_51       "Multichannel \(6 ch\)"                          "5.1 reported as multichannel"
assert surround_51       "Layout \(5.1\) does not match"                  "5.1 layout note (no channel FAIL)"
# Dolby Atmos ADM BWF: bed + objects resolved from chna/axml, no channel-count FAIL.
assert atmos             "7.1.2 bed \+ 2 objects"                         "ADM bed/object layout resolved"
assert atmos             "ADM / Atmos: yes"                               "ADM detected"
assert atmos             "Multichannel \(12 ch\)"                         "atmos multichannel (no channel FAIL)"

# Metadata fixture: tags + artwork must parse (checked via JSON, not the console report).
mdjson="$FX/metadata_QA.json"
"$ARGUS" --json "$FX" "$FX/metadata.wav" >/dev/null 2>&1
if grep -q '"ISRC":"GBTEST2600001"' "$mdjson" 2>/dev/null && \
   grep -q '"artwork":true' "$mdjson" 2>/dev/null; then
  echo "  PASS  embedded tags + artwork parsed"; pass=$((pass+1))
else
  echo "  FAIL  embedded tags + artwork parsed"; fail=$((fail+1))
fi

echo
echo "Results: $pass passed, $fail failed"
[[ $fail -eq 0 ]]

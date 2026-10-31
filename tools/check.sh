#!/bin/bash
# Quick integrity check: runs the build(s) in an invisible, silent Quick Race and
# checks the counters in the end-of-run report.  The one-command answer to
# "does the game still get from the menu to a race, are translation / GPU / audio
# clean" after a change (for the ISA and function tests: ./run.sh test).
#
#   ./tools/check.sh              both builds (out/nfsu2_boot, out/nfsu2_hd_boot)
#   ./tools/check.sh orig|hd      a single build
#
# NFSU2_DATA: the game folder or SPEED2.EXE (same as run.sh).  The window is never
# visible (NOFOCUS=2), sound is off, settings (~/.nfsu2_registry) are restored
# after the run, saves go to a temporary folder (real career saves are not
# touched).  One build takes ~1.5 min.  Exit code: 0 = all passed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXE="$ROOT/Need for Speed Underground 2 - Clean Edition/Need for Speed Underground 2/SPEED2.EXE"
if [ -n "${NFSU2_DATA:-}" ]; then
  if [ -d "$NFSU2_DATA" ]; then EXE="$NFSU2_DATA/SPEED2.EXE"; else EXE="$NFSU2_DATA"; fi
fi
[ -f "$EXE" ] || { echo "SPEED2.EXE not found: $EXE"; exit 2; }
case "${1:-both}" in
  both) BUILDS="orig hd" ;; orig|hd) BUILDS="$1" ;;
  *) echo "usage: tools/check.sh [orig|hd]"; exit 2 ;;
esac

TMP="$(mktemp -d "${TMPDIR:-/tmp}/nfsu2_check.XXXXXX")"
REG="$HOME/.nfsu2_registry"
[ -f "$REG" ] && cp "$REG" "$TMP/registry.backup"
restore(){ [ -f "$TMP/registry.backup" ] && cp "$TMP/registry.backup" "$REG"; }
trap restore EXIT

# From the menus to Quick Race (see PROJECT.md / the quick-race recipe), throttle after 4700.
SEQ="1000:0x1C,1350:0x1C,1700:0x1C,2050:0x1C,2400:0x1C,2750:0x1C,3100:0x1C,3700:0x1C,4000:0x1C,4700:0xC8:2200"
Q=${CHECK_FRAMES:-7000}   # CHECK_FRAMES / CHECK_SEQ: longer soak runs
SEQ=${CHECK_SEQ:-$SEQ}
FAILED=0

check(){   # $1 name, $2 condition (0/1), $3 detail
  if [ "$2" = 1 ]; then printf "  PASS  %-36s %s\n" "$1" "$3"
  else printf "  FAIL  %-36s %s\n" "$1" "$3"; FAILED=1; fi
}
num(){ grep -a -E "$1" "$LOG" | tail -1 | sed -E "s|.*$2.*|\\1|"; }

for b in $BUILDS; do
  BIN="$ROOT/out/nfsu2_boot"; [ "$b" = hd ] && BIN="$ROOT/out/nfsu2_hd_boot"
  [ -x "$BIN" ] || { echo "== $b: $BIN missing (./build.sh && ./link.sh)"; FAILED=1; continue; }
  LOG="$TMP/$b.log"; mkdir -p "$TMP/saves_$b"
  echo "== $b ($(basename "$BIN"), $Q frames)"
  env -i HOME="$HOME" PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" \
    FIXMAT=1 FIXMAT_MODE=3 NOMOVIES=1 SNDSYNC=1 GPUVS=1 WINDOW=1 ALARM=0 \
    NOFOCUS=2 NOSOUND=1 NFSFULL=0 MOUSEAT=5,5 FSFAKE=300 \
    NFSU2_SAVEDIR="$TMP/saves_$b" AUTOSEQ="$SEQ" QUITFRAMES=$Q GPUWIN=6000,6900 \
    ${NFSU2_HEAP_FREE:+NFSU2_HEAP_FREE="$NFSU2_HEAP_FREE"} ${NFSU2_HEAP_QUAR_MB:+NFSU2_HEAP_QUAR_MB="$NFSU2_HEAP_QUAR_MB"} ${HEAPPOISON:+HEAPPOISON="$HEAPPOISON"} ${GBUF_SKIP_MB:+GBUF_SKIP_MB="$GBUF_SKIP_MB"} ${NFSU2_GBUF_MERGE:+NFSU2_GBUF_MERGE="$NFSU2_GBUF_MERGE"} ${NFSU2_GBUF_QUAR:+NFSU2_GBUF_QUAR="$NFSU2_GBUF_QUAR"} ${NFSU2_GBUF_FREE0:+NFSU2_GBUF_FREE0="$NFSU2_GBUF_FREE0"} ${NFSU2_HEAPLOG:+NFSU2_HEAPLOG="$NFSU2_HEAPLOG"} ${THREADLOG:+THREADLOG="$THREADLOG"} \
    "$BIN" "$EXE" > "$LOG" 2>&1
  rc=$?
  restore

  check "run completed" "$([ $rc = 0 ] && grep -a -q "^  unsupported instructions" "$LOG" && echo 1 || echo 0)" "exit $rc"
  v=$(num "unsupported instructions" ': ([0-9]+)'); check "unsupported instructions = 0" "$([ "${v:-x}" = 0 ] && echo 1 || echo 0)" "${v:-?}"
  v=$(num "indirect calls unresolved" ': ([0-9]+)'); check "unresolved indirect calls = 0" "$([ "${v:-x}" = 0 ] && echo 1 || echo 0)" "${v:-?}"
  v=$(num "\[matrix\]" 'broken=([0-9]+).*'); check "broken matrices = 0" "$([ "${v:-x}" = 0 ] && echo 1 || echo 0)" "${v:-?}"
  v=$(num "command buffer error: [0-9]+ frames" 'error: ([0-9]+) frames'); check "GPU command buffer errors = 0" "$([ "${v:-x}" = 0 ] && echo 1 || echo 0)" "${v:-?}"
  # guest heap (gheap.c): no failed allocations, no bad frees
  check "heap: no failed allocations / bad frees" "$(grep -a -E "^  \[heap\] live" "$LOG" | tail -1 | grep -q "0 double / 0 bad / 0 corrupt frees, 0 failed" && echo 1 || echo 0)" "$(grep -a -E "^  \[heap\] live" "$LOG" | tail -1 | sed -E 's/.*; ([0-9]+ double.*failed).*/\1/')"
  # buffer heap (com.c gbuf): no bad / double frees, no out-of-memory
  v=$(num "^  buffer heap +: top" ' ([0-9]+) bad frees'); o=$(grep -a -c "\[gbuf\] OUT OF MEMORY" "$LOG")
  check "buffer heap: 0 bad frees, no OOM" "$([ "${v:-x}" = 0 ] && [ "$o" = 0 ] && echo 1 || echo 0)" "${v:-?} bad, $o OOM"
  v=$(num "[0-9]+ MISSING" ', ([0-9]+) MISSING'); check "missing mip levels = 0" "$([ "${v:-x}" = 0 ] && echo 1 || echo 0)" "${v:-?}"
  v=$(grep -a -E "^ +vs[0-9]+: version" "$LOG" | grep -a -v -c ", 0 unknown"); check "unknown in vertex shaders = 0" "$([ "$v" = 0 ] && echo 1 || echo 0)" "$v shaders"
  v=$(num "sound cursor overrun" ': ([0-9]+) callbacks'); check "audio: cursor overrun = 0" "$([ "${v:-0}" = 0 ] && echo 1 || echo 0)" "${v:-(sound off)}"
  # did we really get into a race: draws per frame in the measurement window (menu ~475, race ~900+)
  v=$(num "draw calls: [0-9]+/frame" 'draw calls: ([0-9]+)/frame'); check "got into a race (draws/frame > 700)" "$([ "${v:-0}" -gt 700 ] && echo 1 || echo 0)" "${v:-?}"
  if [ "$b" = hd ]; then
    v=$(num "\[HD\] ray tracing: [0-9]+ frames" 'tracing: ([0-9]+) frames'); check "HD: ray tracing ran" "$([ "${v:-0}" -gt 500 ] && echo 1 || echo 0)" "${v:-0} frames"
    v=$(num "\[HD\] MSAA attachments" 'mispredicted ([0-9]+).*'); check "HD: memoryless MSAA prediction held" "$([ "${v:-x}" = 0 ] && echo 1 || echo 0)" "${v:-?}"
    v=$(num "\[HD\] separate G pass" 'ON, ([0-9]+) passes.*'); check "HD: separate G pass ran" "$([ "${v:-0}" -gt 500 ] && echo 1 || echo 0)" "${v:-0} passes"
  fi
done
echo
if [ $FAILED = 0 ]; then echo "RESULT: all passed  (logs: $TMP)"; else echo "RESULT: SOME FAILED  (logs: $TMP)"; fi
exit $FAILED

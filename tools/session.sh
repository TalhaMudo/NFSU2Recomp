#!/bin/bash
# Real game session recording (while the user plays).
#
#   ./tools/session.sh            normal build
#   ./tools/session.sh hd         HD build
#   ./tools/session.sh hd all     HD build with every HD picture / reflection option on
#
# Besides PERFLOG every session records (light enough to play with):
#   game log [game t]  GAMELOG (front-end transitions, on the PERFLOG clock)
#   sound              SNDSTAT/SNDGAP lines, sound.csv (every mixer callback),
#                      sound_out.raw (what was played: s16le 44100 Hz stereo)
#   timing / threads   TIMELOG, THREADLOG, GILLOG=20 (lock holds > 20 ms), [timer]
#   files              FILEOPLOG (opens/closes; loads between menus)
#   GPU                PASSPROF every 120 frames -> passprof.csv, MATLOG, TEXUPSAY
#   SESSION_LIGHT=1    PERFLOG only, as before
#
# The game starts with recording on (PERFLOG); play normally, quit the game when
# done.  At the same time powermetrics writes CPU/GPU watts once a second (the
# admin password is asked once; without it only the in-game recording is taken).
# Recording: ~/nfs_sessions/<date>[_hd]   Analysis: python3 tools/session_report.py <dir>
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HD=0; ALL=0
for a in "$@"; do case "$a" in hd) HD=1 ;; all) ALL=1 ;; *) echo "unknown: $a"; exit 1 ;; esac; done
# the recording directory is tagged with the version: always clear which version was played
VERSION="$(tr -cd '0-9' < "$ROOT/VERSION" 2>/dev/null)"; VERSION="${VERSION:-0}"
SUF="_v$VERSION"; ENG=original
[ $HD = 1 ] && { SUF="${SUF}_hd"; ENG=hd; }
# NFSU2.app holds both engines (launcher picks one); an older separate "NFSU2 HD.app"
# is used only when NFSU2.app has no HD engine.
# the bundle with the NEWEST engine (an old out/personal copy must not win over a fresh ./pack.sh)
pick(){ local best="" d
  for d in "$ROOT/out/personal" "$ROOT/out"; do
    [ -d "$d/$1" ] || continue
    if [ -z "$best" ] || [ "$d/$1/Contents/MacOS/nfsu2-engine" -nt "$best/Contents/MacOS/nfsu2-engine" ]; then best="$d/$1"; fi
  done; echo "$best"; }
APP="$(pick NFSU2.app)"
if [ $HD = 1 ] && { [ -z "$APP" ] || [ ! -x "$APP/Contents/MacOS/nfsu2-engine-hd" ]; }; then
  APP="$(pick "NFSU2 HD.app")"
fi
[ -n "$APP" ] || { echo "no app (run ./pack.sh --embed first)"; exit 1; }
DIR="$HOME/nfs_sessions/$(date +%Y%m%d_%H%M%S)$SUF"
mkdir -p "$DIR"
echo "app       : $APP"
echo "recording : $DIR"

PM=0; KA=0
echo "admin password for powermetrics (CPU/GPU watt measurement):"
if sudo -v; then
  sudo powermetrics --samplers cpu_power,gpu_power,thermal -i 1000 -o "$DIR/power.txt" >/dev/null 2>&1 &
  PM=1
  ( while sleep 50; do sudo -n -v 2>/dev/null || exit 0; done ) & KA=$!
else
  echo "warning: no password given, no powermetrics; in-game recording only"
fi

# Temperature / fan (no sudo needed): so the "86 degrees" the user sees is measured directly
SMC="$ROOT/out/smcread"; SM=0
[ "$SMC" -nt "$ROOT/tools/smcread.c" ] || clang -O1 -framework IOKit -o "$SMC" "$ROOT/tools/smcread.c" 2>/dev/null
if [ -x "$SMC" ]; then "$SMC" -k 1000 "$DIR/temperature.csv" & SM=$!; fi

echo
echo "starting the game: play normally (~5 min), quit the game when done."
# Battery / power mode: some performance drops come from the system, not the game
# (2026-10-18: after waking from sleep at 4% battery and plugging in, CPU+GPU were throttled).
{ date; pmset -g batt; pmset -g | grep -i lowpowermode; } > "$DIR/battery_start.txt" 2>&1
# DLINK=2: writes the real display refresh rate to the game's log every 5 s
# NFSU2_SKIP_LAUNCHER: straight into the game, no launcher window
EXTRA=()
if [ "${SESSION_LIGHT:-0}" != 1 ]; then
  for kv in GAMELOG=1 TIMELOG=1 THREADLOG=1 FILEOPLOG=1 GILLOG=20 SNDSTAT=1 SNDGAP=1 \
            "SNDCSV=$DIR/sound.csv" "SNDDUMP=$DIR/sound_out.raw" \
            PASSPROF=120 "PASSPROF_CSV=$DIR/passprof.csv" GPUWIN=1,2000000000 MATLOG=1 TEXUPSAY=1; do
    EXTRA+=(--env "$kv"); done
fi
if [ $ALL = 1 ]; then
  for kv in TINT=none CONSOLEGAMMA=1 EDGEAA=1 SHARPEN=50 \
            NFSU2_REFLECT_SKY=1 NFSU2_REFLECT_LIGHTS=1 NFSU2_REFLECT_DETAIL=1; do
    EXTRA+=(--env "$kv"); done
fi
printf '%s\n' ${EXTRA[@]+"${EXTRA[@]}"} | grep -v '^--env$' > "$DIR/session_env.txt"
open -W -n "$APP" --env NFSU2_SKIP_LAUNCHER=1 --env NFSU2_ENGINE=$ENG \
  --env PERFLOG="$DIR" --env DLINK=2 ${EXTRA[@]+"${EXTRA[@]}"} --stderr "$DIR/game.log"
{ date; pmset -g batt; pmset -g | grep -i lowpowermode; } > "$DIR/battery_end.txt" 2>&1
LG=nfsu2; [ $HD = 1 ] && LG="$LG-hd"
cp "$HOME/Library/Logs/NFSU2/$LG.log" "$DIR/game_full.log" 2>/dev/null

[ $SM != 0 ] && kill $SM 2>/dev/null
if [ $PM = 1 ]; then
  sudo -n pkill -INT -f "powermetrics.*$DIR" 2>/dev/null || sudo pkill -INT -f "powermetrics.*$DIR"
  [ $KA != 0 ] && kill $KA 2>/dev/null
fi
sleep 1
echo
echo "recording done: $DIR ($(du -sh "$DIR" | cut -f1))"
echo "You can tell Claude this directory."

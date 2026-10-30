#!/bin/bash
# Run the NFSU2 native ARM64 port
ROOT="$(cd "$(dirname "$0")" && pwd)"
# NFSU2_DATA: the game folder (containing SPEED2.EXE) or the SPEED2.EXE path itself.
# If not given, the default folder at the repository root is used.
EXE="$ROOT/Need for Speed Underground 2 - Clean Edition/Need for Speed Underground 2/SPEED2.EXE"
if [ -n "$NFSU2_DATA" ]; then
  if [ -d "$NFSU2_DATA" ]; then EXE="$NFSU2_DATA/SPEED2.EXE"; else EXE="$NFSU2_DATA"; fi
fi
[ -f "$EXE" ] || { echo "SPEED2.EXE not found: $EXE"; exit 1; }
# NFSU2_REGION=na|eu: the game's region.  Default: detected from the data's region
#   cars (CARS/CIVIC + RSX without CORSA + PEUGOT106 = North American, e.g. US discs).
#   See runtime/overrides.c, region_install.
# TWO BUILDS (see link.sh):
#   ./run.sh [mode]          original  (out/nfsu2_boot)
#   NFSHD=1 ./run.sh [mode]  HD        (out/nfsu2_hd_boot) -- graphics improvements
#   ./run.sh hd | hd1080p    shortcuts for HD
# HD settings (read by the HD build only): HDREFL=0 (reflection improvement off),
#   HD_ROAD (road reflection scale; default 1/3 with ray tracing on, 0.5 with it off),
#   HD_BLUR=0.5, HD_MIRROR=1024, HD_CUBE=512,
#   HD_MSAA (default automatic: 2x at 1440 rows and above, 4x below; 1 = off),
#   HD_SSR=1 (0 off), HD_SSR_CAR=1 (car paint), HD_RT=1 (ray-traced reflections; 0 = SSR),
#   HD_GSEP=1 (G-buffer in a separate single-sampled pass), HD_MBFIX=1 (the scene stays
#   multisampled during motion blur), HD_RT_FAR / HD_RT_BMIN (ray limits; off by default).
#   Details: runtime/enhance.c, runtime/metal.m (sections of the same name)
# Picture options (the launcher's Picture card; unset = the original game):
#   TINT=less|none|cool    the screen filter's yellow-green tint: half / none / none + cooler
#   Details: runtime/visual.c
# In game, Cmd+Shift+D: a PNG of that frame + the draw list (~/Library/Logs/NFSU2/
#   or the PERFLOG directory).  Measurement recording while playing: tools/session.sh [hd].
MODE="${1:-window}"
case "$MODE" in
  hd)      NFSHD=1; MODE=window ;;
  hd1080p) NFSHD=1; MODE=1080p ;;
esac
# The engine must match the executable: the North American v1.2 SPEED2.EXE runs on
# out/nfsu2_na[_hd]_boot (BUILD=na ./build.sh && BUILD=na ./link.sh).
NA=""
[ "$(shasum -a 256 "$EXE" | cut -c1-64)" = f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7 ] && NA="_na"
BIN="$ROOT/out/nfsu2${NA}_boot"
[ "${NFSHD:-0}" = 1 ] && BIN="$ROOT/out/nfsu2${NA}_hd_boot"
[ -n "$NA" ] && echo "SPEED2.EXE: North American v1.2 -> $(basename "$BIN")"
[ -x "$BIN" ] || { echo "build first: ./build.sh && ./link.sh  ($BIN missing)"; exit 1; }
[ "${NFSHD:-0}" = 1 ] && echo "build: HD (graphics improvements on)" || echo "build: original"

# --- temporarily required settings (root causes still open) ---
# FIXMAT   : the game's front-end WorldViewProj matrix arrives broken (only
#            rows 2 and 3 written, 0-1 zero) -> every vertex collapses to
#            (1,1).  MODE=3 injects the correct identity matrix; the raw
#            positions are already in clip space.
# NOMOVIES : 1 skips the videos (they play by default: the game decodes VP6
#            itself; the old hang at the first video was the movie timer's
#            CPU calibration, fixed with the "~MHz" registry value).
# Don't let the diagnostic synthetic input/forcing variables leak in from the
# shell: an old "export AUTOKEY=..." made the game look like it clicked by itself.
unset AUTOKEY AUTOCLICK FORCESWITCH FETICK GAMELOG DUMPFRAME BOOTFLOW \
      DATADIFF VSKDUMP OWNERSCAN PARAMNAME PDLIST WATCHVSK WATCHADDR \
      MPROT_HARD MPROT_ANY MPROT_PERSIST PRESENTBT VSKBT GLYPHDUMP DRAWDUMP \
      AUTOSEQ AUTOTEXT GILLOG SNDSTAT SNDGAP SNDLOG \
      READLOG GPUVSLOG GPUVSDUMP GPUVSMASK NOVS NOPACE PACE60 TESTFILL \
      SKIPDRAW CLEARCOL MIPOFF DRAWV SLIVER PSDUMP DUMPMIP DUMPTEX SMPLOG LOCKLOG RTLOG \
      DRAWSUM PSISO SKIPPS PSINFO BLITLOG SNDCSV SNDDUMP SNDBACKEND \
      FPSCSV AUTOKEYSTOP VCLOCK VCLOCK_MAXD DECLLOG CULLWIND RSLOG NOFOG BLITQUAD RESSCAN ASPFIX ASPLOG PSHIST MIPLIN NOANISO LODMAX \
      RTIDLOG DRAWVPS ZSHARELOG XFORMAT CAMLOG HD_SSR_DEBUG VIEWLOG HD_RTPROBE HD_RTPROBE2 HD_RT_STAGE HD_RT_NOTEX HD_RT_PLAIN HD_RT_ONLYCX HD_RT_SLOWBUILD HD_RT_LOG HD_RT_DUMPTAB HD_RT_STRESS HD_COMP HD_MSLAZY HD_REORDER HD_RT_AB HD_RT_FIX HD_RT_POISON FORCERAIN FORCETOD FORCETOD_STATE GPUWIN PASSPROF PASSPROF_MODE GPUVSDUMP \
      SNDMUTE YAKALAKARE DRAWLIST NODRAW LIMFREE ABTEST PASSPROF_CSV HD_MBLOG HD_RT_DLOG HD_RT_ONLY HD_RTPROBE2_HER \
      STALL MEMDUMP REGFIRST ADAPTERID FPSCAP BGFPS FBONLY

export FIXMAT="${FIXMAT:-1}"
export NOMOVIES="${NOMOVIES:-0}"
export FIXMAT_MODE="${FIXMAT_MODE:-3}"
# SNDSYNC : tie the playback rate to the ratio of the game's virtual clock to
#           real time.  Since the clock now advances with real elapsed time the
#           ratio is normally 1.000; it only drops briefly during a long hitch
#           (so unwritten data isn't played).  0 = always play in real time.
# SNDPLL  : the old write-cursor servo (OFF by default).  Since the game picks
#           where it writes based on our playback cursor, the servo couldn't
#           close the error and played the music 3% slow/fast for 40-50 s
#           (see runtime/audio.m).
export SNDSYNC="${SNDSYNC:-1}"
# GPUVS : run the vertex shader on the GPU (the game's own bytecode is
#         translated to MSL).  Measured: draw path 34.6 s -> 2.2 s,
#         vertex throughput 2.4 -> 90 M/s.  A shader that can't be translated
#         automatically falls back to the CPU interpreter.
#         GPUVS=0 goes back to the old (CPU) path.
export GPUVS="${GPUVS:-1}"

# NOTE: this used to say "the front end hangs at UG_LS_Splash.fng, FORCESWITCH=3
# is needed".  That is NO LONGER TRUE: after the FPUSH fix the game reaches the
# title screen by itself and waits with "Press Enter Key" -- exactly the original
# behavior.  FORCESWITCH=3 is only kept for skipping to the menu without Enter
# (measurement/diagnostics).
# NFSRES: the game's own resolution table has 6 rows, all 4:3 -- 1080p can't be
# picked from the menu.  This variable opens the device at the requested size,
# corrects the geometry for Hor+ widescreen and keeps the UI round (turned off
# with HUDFIX=0).
#   NFSRES=1920x1080 ./run.sh      or     ./run.sh 1080p
export NFSRES="${NFSRES:-}"
# NFSNATIVE: on a Retina screen, raises the resolution the game picked (at most
#   1280x960 in the menu) to the screen's real pixel count KEEPING THE RATIO
#   (2448x1836 on a 14" MacBook Pro).  The 1280x960 surface used to be scaled
#   up ~2x to the window and got blurry.  ON by default.  NFSNATIVE=0 turns it
#   off: the image is still 1:1 pixels but the window opens at half size.
#   If NFSRES is given, it wins.
# NFSMAXPIX: upper limit of the native scale, in pixels (default 6000000;
#   to protect the frame budget on 5K/6K screens).

# For a double-clickable .app:  ./pack.sh  (see out/NFSU2.app)

if [ "$MODE" = "1080p" ]; then MODE=window; export NFSRES="${NFSRES:-1920x1080}"; fi
[ -n "$NFSRES" ] && echo "resolution: $NFSRES (widescreen correction on)"
case "$MODE" in
  window)   echo "windowed mode (close the window or Ctrl-C to quit)"
            WINDOW=1 ALARM="${ALARM:-0}" exec "$BIN" "$EXE" ;;
  headless) echo "headless mode, ${ALARM:-60} s"
            ALARM="${ALARM:-60}" exec "$BIN" "$EXE" ;;
  profile)  echo "sampling profiler"
            SAMPLE=1 ALARM="${ALARM:-30}" exec "$BIN" "$EXE" ;;
  trace)    echo "D3D9 call trace"
            COMTRACE=1 ALARM="${ALARM:-20}" exec "$BIN" "$EXE" ;;
  test)     "$ROOT/out/isatest" && "$ROOT/out/nfsu2_all" "$EXE" ;;
  *)        echo "usage: [NFSHD=1] ./run.sh [window|1080p|hd|hd1080p|headless|profile|trace|test]"; exit 1 ;;
esac

#!/bin/bash
# Compile the runtime and link the binaries (the shards must already be compiled)
#
# TWO SEPARATE BUILDS are produced; the lifted shards are SHARED, only the runtime differs:
#   out/nfsu2_boot     original  -- the game's own look, a faithful port
#   out/nfsu2_hd_boot  HD        -- graphics improvements (runtime/enhance.*,
#                                   NFS_ENHANCED=1).  See GRAPHICS_RESEARCH.md
#   out/nfsu2_all      test suite (with the original runtime)
#
#   ./link.sh          both
#   ./link.sh orig     original only
#   ./link.sh hd       HD only
#
# BUILD=na: the North American v1.2 executable (README "Game versions").
#   Shards from gen/na -> out/obj_na (BUILD=na ./build.sh), the runtime compiled
#   with -DNFS_EXE_NA=1 and runtime/build_na.h (the reference build's addresses
#   mapped to this build, tools/addrmap.py), binaries out/nfsu2_na_boot and
#   out/nfsu2_na_hd_boot.  No test suite binary.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
MCPU="${MCPU:-apple-m1}"; MINOS="${MINOS:-11.0}"   # see build.sh
OPT="${OPT:--O3 -mcpu=$MCPU -mmacosx-version-min=$MINOS -fno-strict-aliasing}"   # see build.sh
LDOPT="${LDOPT} -mmacosx-version-min=$MINOS"
WHICH="${1:-both}"
# Version tracking: the VERSION file at the repository root (v1 archive: arsiv/v1/).
# The runtime prints it at startup and in PERFLOG meta.txt, pack.sh puts it in the
# bundle's version info.
VERSION="$(tr -cd '0-9' < "$ROOT/VERSION" 2>/dev/null)"; VERSION="${VERSION:-0}"
case "$WHICH" in both|orig|hd) ;; *) echo "usage: ./link.sh [orig|hd]"; exit 1 ;; esac

case "${BUILD:-eu}" in
  eu) GEN=gen/all; SOBJ=out/obj; RO=out/obj; RH=out/obj_hd; XDEF=""
      BOOT=out/nfsu2_boot; HDBOOT=out/nfsu2_hd_boot ;;
  na) GEN=gen/na;  SOBJ=out/obj_na; RO=out/obj_na_rt; RH=out/obj_na_hd; XDEF="-DNFS_EXE_NA=1 -include runtime/build_na.h"
      BOOT=out/nfsu2_na_boot; HDBOOT=out/nfsu2_na_hd_boot
      # keep the address header in step with the runtime when the map is here
      # (it needs both executables to build; the committed header is used otherwise)
      if [ -f out/na/addrmap.json ]; then
        "$ROOT/.venv/bin/python" tools/addrmap.py header out/na/addrmap.json runtime/build_na.h \
          runtime/*.c runtime/*.m runtime/*.h tests/boot.c
      fi ;;
  *)  echo "BUILD must be eu or na"; exit 1 ;;
esac

CSRC="mem dispatch genexec win32 win32_ext com debug res fileio overrides d3dx_hle vsemu enhance perflog wsfov gheap sndlead xpatch highfps xtinput"
MSRC="metal audio bundle gamepad"
FW="-lm -framework Cocoa -framework Metal -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreServices -framework AudioToolbox -framework GameController -framework CoreText"
SHARDS="$SOBJ/shard_*.o $SOBJ/fntab.o $SOBJ/stubs.o"

# $1 = object directory, $2 = NFS_ENHANCED value.  Files are compiled in parallel.
build_rt(){
  local od="$1" enh="$2" pids="" f
  mkdir -p "$od"
  for f in $CSRC; do
    [ -f "runtime/$f.c" ] || continue
    clang $OPT $XDEF -DNFS_ENHANCED=$enh -DNFS_VERSION=$VERSION -Iruntime -I$GEN -c -o "$od/$f.o" "runtime/$f.c" & pids="$pids $!"
  done
  for f in $MSRC; do
    clang $OPT $XDEF -DNFS_ENHANCED=$enh -DNFS_VERSION=$VERSION -fobjc-arc -Iruntime -I$GEN -c -o "$od/$f.o" "runtime/$f.m" & pids="$pids $!"
  done
  clang $OPT -Iruntime -I$GEN -c -o "$od/impargs.o" $GEN/impargs.c & pids="$pids $!"
  clang $OPT $XDEF -DNFS_ENHANCED=$enh -Iruntime -I$GEN -c -o "$od/boot.o" tests/boot.c & pids="$pids $!"
  for p in $pids; do wait "$p" || { echo "compile error ($od)"; return 1; }; done
}
rt_objs(){ local od="$1" f o=""
  for f in $CSRC $MSRC impargs; do [ -f "$od/$f.o" ] && o="$o $od/$f.o"; done
  echo "$o"; }

if [ "$WHICH" != hd ]; then
  build_rt $RO 0
  rm -f $RO/enhance.o                # not part of the original build
  RT="$(rt_objs $RO)"
  clang ${LDOPT} -o $BOOT $SHARDS $RT $RO/boot.o $FW
  if [ "${BUILD:-eu}" = eu ]; then
    clang $OPT -Iruntime -Igen/all -c -o out/obj/main.o tests/main_all.c
    clang ${LDOPT} -o out/nfsu2_all  $SHARDS $RT out/obj/main.o $FW
  fi
  echo "link OK: original ($BOOT)"
fi
if [ "$WHICH" != orig ]; then
  build_rt $RH 1
  RT="$(rt_objs $RH)"
  clang ${LDOPT} -o $HDBOOT $SHARDS $RT $RH/boot.o $FW
  echo "link OK: HD ($HDBOOT)"
fi
[ "${BUILD:-eu}" = eu ] || exit 0

# ISA semantic tests
# DON'T PASS SILENTLY if the generator fails.  The error used to be swallowed:
# while isatest.py crashed for weeks the old gen/isatest.c got compiled and said
# "439 passed", i.e. the tests never exercised the current lifter.  Now the stale
# binary is deleted so ./run.sh test fails visibly.
if "$ROOT/.venv/bin/python" "$ROOT/tools/isatest.py" >/dev/null \
   && clang -O1 -Wno-unused-label -Iruntime -o out/isatest gen/isatest.c runtime/mem.c -lm; then
  :
else
  rm -f out/isatest
  echo "WARNING: ISA tests could not be generated/compiled -- out/isatest deleted" >&2
fi

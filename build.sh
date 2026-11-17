#!/bin/bash
# Compile the 23k lifted functions in parallel
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
# -O3 + -mcpu=native measured: race frame median 18.7 -> 15.7 ms
# (53.5 -> 63.9 fps).  Lifted code consists of many small basic blocks, more
# aggressive inlining/simplification pays off directly.
# DISTRIBUTION baseline:
#  -mcpu=apple-m1  : M1 is the common denominator of all Apple Silicon.  If built
#                    with "native", a binary produced on a newer Mac crashes on M1
#                    with an illegal instruction.  The measured difference on M1 is zero.
#  -mmacosx-version-min=11.0 : without the flag the binary is stamped with the
#                    building machine's OS version (minos 26.0) and on older macOS
#                    it never opens, saying "a newer version is required".
MCPU="${MCPU:-apple-m1}"
MINOS="${MINOS:-11.0}"
# -fno-strict-aliasing is MANDATORY (default since v2): lifted code accesses guest
# memory at the same address with different types (MR32 write, MRF32 read);
# without the flag clang's GVN can miscompile it.  See docs/lifter-efficiency.md section 4.
OPT="${OPT:--O3 -mcpu=$MCPU -mmacosx-version-min=$MINOS -fno-strict-aliasing}"
J="${J:-$(sysctl -n hw.ncpu)}"
# BUILD=eu (default): gen/all -> out/obj    BUILD=na: gen/na -> out/obj_na
# (North American v1.2 executable; see README "Game versions", tools/lifter/gen_v2.sh)
case "${BUILD:-eu}" in
  eu) GEN=gen/all; OBJ=out/obj ;;
  na) GEN=gen/na;  OBJ=out/obj_na ;;
  *)  echo "BUILD must be eu or na"; exit 1 ;;
esac
mkdir -p "$ROOT/$OBJ"
cd "$ROOT/$GEN"
# DON'T EMBED $OPT in the xargs command line: the length of a line built with
# -I{} is limited ("command line cannot be assembled, too long").  Pass it in
# from the environment and let the inner shell expand it.
#
# DON'T PIPE the compiler output STRAIGHT into "| head -5": when head closed after
# 5 lines clang died of SIGPIPE while writing the next warning and the object file
# was NEVER written (shard_0006/0019: 3 times in 3 tries), and build.sh still
# returned 0 -> the link broke with "symbol(s) not found".  The output is first
# captured in a variable, then truncated; clang's exit code is kept (if any shard
# fails to compile, xargs and therefore build.sh return an error).
export OPT ROOT OBJ
ls shard_*.c fntab.c stubs.c 2>/dev/null | \
  xargs -P "$J" -n 1 sh -c '
    o=$(clang $OPT -c -I"$ROOT/runtime" -I. -Wno-unused-label -Wno-parentheses-equality \
          -o "$ROOT/$OBJ/$(basename "$1" .c).o" "$1" 2>&1); r=$?
    [ -z "$o" ] || printf "%s\n" "$o" | head -5
    [ $r -eq 0 ] || echo "COMPILE FAILED: $1" >&2
    exit $r' sh

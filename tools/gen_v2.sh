#!/bin/bash
# Generate the v2 lifted code: SPEED2.EXE -> gen/<build> (then ./build.sh && ./link.sh)
#
#   tools/gen_v2.sh <SPEED2.EXE>                  generates into gen/all (European v1.2)
#                                                 or gen/na (North American v1.2),
#                                                 chosen by the file's SHA-256
#   GENOUT=<dir> tools/gen_v2.sh <SPEED2.EXE>     into another directory (for
#                                                 verification; the impargs.c step is skipped)
#
# v2 = v1 (base translator) + FLAGABI + X87OPT + RETEIP=0 + MEMARG=1 and the whole
# build with -fno-strict-aliasing (build.sh / link.sh).  Details and measurements:
# LIFTER_EFFICIENCY.md.  v1 archive: arsiv/v1/.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
EXE="${1:?usage: tools/gen_v2.sh <SPEED2.EXE>}"
PY="$ROOT/.venv/bin/python"

# The OVERRIDE list must MATCH runtime/overrides.c + runtime/d3dx_hle.c
# (see PROJECT.md "Commands", D3DX_HLE.md "Regeneration").  OVR is in European
# addresses; OVR_NA is the same list mapped to the North American build
# (tools/addrmap.py list out/na/addrmap.json "$OVR").
OVR=60fab0,61e390,61f130,5c7ed0,50d510,65ae35,6623ea,66aa3f,66ace2,6691b1,669296,664a2f,663a42,688e89,663275,662edc,660a52,668e0d,664792,66ac27,65addb,65acf5,66d13a,66d303,66d7e9,66e8a9,66f501,66ca38,65ab24,67193e,66056c,65cf44,640c12,668cd0,65ccd8
OVR_NA=60faa0,61e380,61f120,5c7ec0,50d510,65ae25,6623da,66aa2f,66acd2,6691a1,669286,664a1f,663a32,688e79,663265,662ecc,660a42,668dfd,664782,66ac17,65adcb,65ace5,66d12a,66d2f3,66d7d9,66e899,66f4f1,66ca28,65ab14,67192e,66055c,65cf34,640c02,668cc0,65ccc8

SHA="$(shasum -a 256 "$EXE" | cut -c1-64)"
case "$SHA" in
  8f6a9ae2c4cfa01181d2fd755d99c6f4b7103f60273908ea1de1e719dfe2a297)
    BUILD=eu; DEF=gen/all; WORK=out
    export HOOKS="${HOOKS:-0x5c7ffe,0x4ec35c,0x726336,0x5c6766}" ;;   # runtime/wsfov.c, sndlead.c, visual.c
  f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7)
    BUILD=na; DEF=gen/na; WORK=out/na; OVR=$OVR_NA; export LIFT_MISSING=1
    export HOOKS="${HOOKS:-0x5c7fee,0x4ec35c,0x726336,0x5c6756}" ;;   # runtime/wsfov.c, sndlead.c, visual.c (same sites, NA addresses)
  *) echo "unsupported SPEED2.EXE (sha256 $SHA); see README, \"Game versions\""; exit 1 ;;
esac
OUT="${GENOUT:-$DEF}"
mkdir -p "$WORK"
export RELOCS="$WORK/relocs.txt"
echo "build: $BUILD -> $OUT"

"$PY" tools/reloc2.py "$EXE"                        # -> $RELOCS
"$PY" tools/flagabi.py "$EXE" "$WORK/flagabi.json"  # inter-function flag liveness (~30 s)
OVERRIDE=$OVR GENOUT="$OUT" FLAGABI="$WORK/flagabi.json" X87OPT=1 RETEIP=0 MEMARG=1 \
  "$PY" tools/genall.py "$EXE" 128
# argcount.py writes <GENOUT>/impargs.c; run it only for the real generation
if [ "$OUT" = "$DEF" ]; then GENOUT="$OUT" "$PY" tools/argcount.py "$EXE"; fi
echo "v2 generated: $OUT"

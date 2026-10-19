#!/bin/bash
# v2 lift edilmis kodu uret: SPEED2.EXE -> gen/all (sonra ./build.sh && ./link.sh)
#
#   tools/uret_v2.sh <SPEED2.EXE>                 gen/all'a uretir
#   GENOUT=<dizin> tools/uret_v2.sh <SPEED2.EXE>  baska dizine (dogrulama icin;
#                                                 impargs.c adimi atlanir)
#
# v2 = v1 (temel cevirici) + FLAGABI + X87OPT + RETEIP=0 + MEMARG=1 ve tum
# derleme -fno-strict-aliasing ile (build.sh / link.sh).  Ayrinti ve olcumler:
# LIFTER_VERIMLILIK.md.  v1 arsivi: arsiv/v1/.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
EXE="${1:?kullanim: tools/uret_v2.sh <SPEED2.EXE>}"
PY="$ROOT/.venv/bin/python"
OUT="${GENOUT:-gen/all}"

# OVERRIDE listesi runtime/overrides.c + runtime/d3dx_hle.c ile AYNI olmali
# (bkz. PROJECT.md "Komutlar", D3DX_HLE.md "Yeniden uretim").
OVR=60fab0,61e390,61f130,5c7ed0,50d510,65ae35,6623ea,66aa3f,66ace2,6691b1,669296,664a2f,663a42,688e89,663275,662edc,660a52,668e0d,664792,66ac27,65addb,65acf5,66d13a,66d303,66d7e9,66e8a9,66f501,66ca38,65ab24,67193e,66056c,65cf44,640c12,668cd0,65ccd8

"$PY" tools/reloc2.py "$EXE"                    # -> out/relocs.txt
"$PY" tools/flagabi.py "$EXE" out/flagabi.json  # fonksiyonlar arasi bayrak canliligi (~30 sn)
OVERRIDE=$OVR GENOUT="$OUT" FLAGABI=out/flagabi.json X87OPT=1 RETEIP=0 MEMARG=1 \
  "$PY" tools/genall.py "$EXE" 128
# argcount.py her zaman gen/all/impargs.c'ye yazar; yalnizca asil uretimde calistir
if [ "$OUT" = gen/all ]; then "$PY" tools/argcount.py "$EXE"; fi
echo "v2 uretildi: $OUT"

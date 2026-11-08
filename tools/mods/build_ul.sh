#!/bin/bash
# NFSU2 Unlimiter for the North American engine, from the plugin file to linked code.
#
#   tools/mods/build_ul.sh <NFSU2Unlimiter.asi> <NA SPEED2.EXE> [record]
#
# 1. out/ul/          the plugin moved to 0x0A000000 (asi_rebase.py), its
#                     relocation list and meta.json
# 2. gen/ul/          the plugin lifted to C (own table: g_ul_fn_*), PUSHRET=1
#                     so "push imm; ret" in its code caves is a jump
# 3. out/ul/resume.txt  where those caves jump back into game functions
#                     (asi_resume.py); the NA engine gets them as entry points
# 4. gen/ul/inc/asi_ul.h + tools/mods/xpatch_ul.txt  from [record] (the file
#                     runtime/asi.c writes with ASIREC=...), see asi_sites.py.
#                     Without a record: no sites (first run, to record).
# 5. gen/na regenerated with the resume entries and the xpatch sites
#    (tools/gen_v2.sh), everything compiled: BUILD=na ./build.sh, out/obj_ul.
# Then: BUILD=na ./link.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
ASI="${1:?usage: build_ul.sh <NFSU2Unlimiter.asi> <NA SPEED2.EXE> [record]}"
EXE="${2:?NA SPEED2.EXE}"
REC="${3:--}"
PY="$ROOT/.venv/bin/python"
MCPU="${MCPU:-apple-m1}"; MINOS="${MINOS:-11.0}"
OPT="${OPT:--O3 -mcpu=$MCPU -mmacosx-version-min=$MINOS -fno-strict-aliasing}"

"$PY" tools/mods/asi_rebase.py "$ASI" out/ul
export RELOCS=out/ul/relocs.txt
if [ "${SKIP_UL_LIFT:-0}" != 1 ]; then
  "$PY" tools/flagabi.py out/ul/plugin.dll out/ul/flagabi.json > out/ul/flagabi.log 2>&1
  rm -rf gen/ul
  GENOUT=gen/ul FNTAB_PREFIX=ul_ PUSHRET=1 FLAGABI=out/ul/flagabi.json X87OPT=1 RETEIP=0 MEMARG=1 LIFT_MISSING=1 \
    "$PY" tools/genall.py out/ul/plugin.dll 16 > out/ul/gen.log 2>&1
  grep -a -E "functions  |traps \(" out/ul/gen.log
fi
unset RELOCS
"$PY" tools/mods/asi_resume.py out/ul/plugin.dll "$EXE" out/ul/resume.txt
mkdir -p gen/ul/inc
# xpatch sites: the NA engine's own list + the plugin's recorded ones
"$PY" tools/mods/asi_sites.py out/ul/meta.json "$EXE" gen/na/fntab.c "$REC" tools/mods/xpatch_ul.txt gen/ul/inc/asi_ul.h \
      gen/na/xpatch_data.txt
cat tools/xpatch_na.txt tools/mods/xpatch_ul.txt > out/ul/xpatch_all.txt

if [ "${SKIP_NA_GEN:-0}" != 1 ]; then
  XPATCH_SOFT=ul_ XPATCH_TAILJMP=1 EXTRA_FUNCS="$ROOT/out/ul/resume.txt" XPATCH="$ROOT/out/ul/xpatch_all.txt" tools/gen_v2.sh "$EXE" > out/ul/na_gen.log 2>&1 \
    || { tail -20 out/ul/na_gen.log; exit 1; }
  grep -a -E "xpatch:|MISSING" out/ul/na_gen.log
  # ranges the engine generation found to be data: the runtime table says so
  "$PY" tools/mods/asi_sites.py out/ul/meta.json "$EXE" gen/na/fntab.c "$REC" tools/mods/xpatch_ul.txt \
        gen/ul/inc/asi_ul.h gen/na/xpatch_data.txt
  BUILD=na ./build.sh
fi

# the plugin's shards
mkdir -p out/obj_ul
export OPT ROOT
(cd gen/ul && ls shard_*.c fntab.c stubs.c 2>/dev/null | xargs -P "$(sysctl -n hw.ncpu)" -n 1 sh -c '
  o=$(clang $OPT -c -I"$ROOT/runtime" -I. -Wno-unused-label -Wno-parentheses-equality \
        -o "$ROOT/out/obj_ul/$(basename "$1" .c).o" "$1" 2>&1); r=$?
  [ -z "$o" ] || printf "%s\n" "$o" | head -5
  [ $r -eq 0 ] || echo "COMPILE FAILED: $1" >&2
  exit $r' sh)
echo "plugin built: gen/ul -> out/obj_ul ($(ls out/obj_ul/*.o | wc -l | tr -d ' ') objects)"

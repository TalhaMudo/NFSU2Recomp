#!/bin/bash
# opt6 build'i: verimli lifter kodu + GUNCEL runtime, hepsi -fno-strict-aliasing ile.
#
#   gen/opt6 = FLAGABI + X87OPT + RETEIP + MEMARG  (bkz. LIFTER_VERIMLILIK.md)
#   Temele gore yarista komut -%16.7, cevrim -%19.7, ikili -%25.
#
# Uretim build'ine (gen/all, out/obj, out/nfsu2_boot) DOKUNMAZ; ayri nesne
# dizinleri ve ayri ikililer uretir, iki surum yan yana karsilastirilabilir.
#
#   ./link_opt6.sh          normal + HD
#   ./link_opt6.sh orig     yalnizca normal  -> out/nfsu2_opt6_boot
#   ./link_opt6.sh hd       yalnizca HD      -> out/nfsu2_opt6_hd_boot
#
# Paket: ./pack.sh --opt6 [--hd] [--embed]
#
# -fno-strict-aliasing ZORUNLU: lift kodu misafir bellegine ayni adresten
# farkli tiplerle erisir (MR32 yazip MRF32 okumak).  Bayraksiz derlemede
# clang GVN bunu yanlis derleyebiliyor (opt5'te gorundu, bkz. LIFTER_VERIMLILIK.md
# bolum 4).  Runtime da ayni bayrakla derlenir.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
MCPU="${MCPU:-apple-m1}"; MINOS="${MINOS:-11.0}"   # bkz. build.sh
OPT="-O3 -mcpu=$MCPU -mmacosx-version-min=$MINOS -fno-strict-aliasing"
LDOPT="-mmacosx-version-min=$MINOS"
J="${J:-$(sysctl -n hw.ncpu)}"
WHICH="${1:-both}"
case "$WHICH" in both|orig|hd) ;; *) echo "kullanim: ./link_opt6.sh [orig|hd]"; exit 1 ;; esac

GEN=gen/opt6; OBJ=out/obj_opt6
[ -f "$GEN/fntab.c" ] || { echo "$GEN yok: LIFTER_VERIMLILIK.md'deki tarifle uretin (genall.py MEMARG=1 ...)"; exit 1; }

# --- 1) lift edilmis shard'lar ------------------------------------------
# Yalnizca kaynagi ya da runtime basliklari nesneden yeni olanlar derlenir.
mkdir -p "$OBJ"
HNEW="$(ls -t runtime/*.h | head -1)"
export OPT ROOT OBJ HNEW
(cd "$GEN" && ls shard_*.c fntab.c stubs.c) | \
  xargs -P "$J" -n 1 sh -c '
    s="$ROOT/gen/opt6/$1"; o="$ROOT/$OBJ/$(basename "$1" .c).o"
    if [ -f "$o" ] && [ "$o" -nt "$s" ] && [ "$o" -nt "$ROOT/$HNEW" ]; then exit 0; fi
    m=$(cd "$ROOT/gen/opt6" && clang $OPT -c -I"$ROOT/runtime" -I. -Wno-unused-label \
          -Wno-parentheses-equality -o "$o" "$1" 2>&1); r=$?
    [ -z "$m" ] || printf "%s\n" "$m" | head -5
    [ $r -eq 0 ] || echo "DERLENEMEDI: $1" >&2
    exit $r' sh
SHARDS="$OBJ/shard_*.o $OBJ/fntab.o $OBJ/stubs.o"

# --- 2) runtime (link.sh ile ayni dosyalar, ayni bayrak + -fno-strict-aliasing)
# Runtime yalnizca disa acik sub_X(CPU*) sarmalayicilarini cagirir; bunlar
# opt6'da da ayni imzayla var, bu yuzden basliklar gen/all'dan alinir
# (impargs.c yalnizca orada uretiliyor).
CSRC="mem dispatch genexec win32 win32_ext com debug res fileio overrides d3dx_hle vsemu enhance perflog"
MSRC="metal audio bundle"
FW="-lm -framework Cocoa -framework Metal -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreServices -framework AudioToolbox"
build_rt(){
  local od="$1" enh="$2" pids="" f
  mkdir -p "$od"
  for f in $CSRC; do
    [ -f "runtime/$f.c" ] || continue
    clang $OPT -DNFS_ENHANCED=$enh -DNFS_BUILDTAG='"-opt6"' -Iruntime -Igen/all -c -o "$od/$f.o" "runtime/$f.c" & pids="$pids $!"
  done
  for f in $MSRC; do
    clang $OPT -DNFS_ENHANCED=$enh -DNFS_BUILDTAG='"-opt6"' -fobjc-arc -Iruntime -Igen/all -c -o "$od/$f.o" "runtime/$f.m" & pids="$pids $!"
  done
  clang $OPT -Iruntime -Igen/all -c -o "$od/impargs.o" gen/all/impargs.c & pids="$pids $!"
  clang $OPT -DNFS_ENHANCED=$enh -Iruntime -Igen/all -c -o "$od/boot.o" tests/boot.c & pids="$pids $!"
  for p in $pids; do wait "$p" || { echo "derleme hatasi ($od)"; return 1; }; done
}
rt_objs(){ local od="$1" f o=""
  for f in $CSRC $MSRC impargs; do [ -f "$od/$f.o" ] && o="$o $od/$f.o"; done
  echo "$o"; }

if [ "$WHICH" != hd ]; then
  build_rt out/obj_opt6rt 0
  rm -f out/obj_opt6rt/enhance.o      # orijinal build'e girmez
  clang $LDOPT -o out/nfsu2_opt6_boot $SHARDS $(rt_objs out/obj_opt6rt) out/obj_opt6rt/boot.o $FW
  echo "link OK: opt6 orijinal (out/nfsu2_opt6_boot)"
fi
if [ "$WHICH" != orig ]; then
  build_rt out/obj_opt6rt_hd 1
  clang $LDOPT -o out/nfsu2_opt6_hd_boot $SHARDS $(rt_objs out/obj_opt6rt_hd) out/obj_opt6rt_hd/boot.o $FW
  echo "link OK: opt6 HD (out/nfsu2_opt6_hd_boot)"
fi

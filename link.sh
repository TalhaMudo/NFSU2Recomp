#!/bin/bash
# runtime'i derle ve ikilileri linkle (shard'lar zaten derlenmis olmali)
#
# IKI AYRI BUILD uretilir; lift edilmis shard'lar ORTAK, yalnizca runtime farkli:
#   out/nfsu2_boot     orijinal  -- oyunun kendi goruntusu, sadik port
#   out/nfsu2_hd_boot  HD        -- grafik iyilestirmeleri (runtime/enhance.*,
#                                   NFS_ENHANCED=1).  Bkz. GRAFIK_ARASTIRMA.md
#   out/nfsu2_all      test paketi (orijinal runtime ile)
#
#   ./link.sh          ikisini de
#   ./link.sh orig     yalnizca orijinal
#   ./link.sh hd       yalnizca HD
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
MCPU="${MCPU:-apple-m1}"; MINOS="${MINOS:-11.0}"   # bkz. build.sh
OPT="${OPT:--O3 -mcpu=$MCPU -mmacosx-version-min=$MINOS}"
LDOPT="${LDOPT} -mmacosx-version-min=$MINOS"
WHICH="${1:-both}"
case "$WHICH" in both|orig|hd) ;; *) echo "kullanim: ./link.sh [orig|hd]"; exit 1 ;; esac

CSRC="mem dispatch genexec win32 win32_ext com debug res fileio overrides d3dx_hle vsemu enhance"
MSRC="metal audio bundle"
FW="-lm -framework Cocoa -framework Metal -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreServices -framework AudioToolbox"
SHARDS="out/obj/shard_*.o out/obj/fntab.o out/obj/stubs.o"

# $1 = nesne dizini, $2 = NFS_ENHANCED degeri.  Dosyalar paralel derlenir.
build_rt(){
  local od="$1" enh="$2" pids="" f
  mkdir -p "$od"
  for f in $CSRC; do
    [ -f "runtime/$f.c" ] || continue
    clang $OPT -DNFS_ENHANCED=$enh -Iruntime -Igen/all -c -o "$od/$f.o" "runtime/$f.c" & pids="$pids $!"
  done
  for f in $MSRC; do
    clang $OPT -DNFS_ENHANCED=$enh -fobjc-arc -Iruntime -Igen/all -c -o "$od/$f.o" "runtime/$f.m" & pids="$pids $!"
  done
  clang $OPT -Iruntime -Igen/all -c -o "$od/impargs.o" gen/all/impargs.c & pids="$pids $!"
  clang $OPT -DNFS_ENHANCED=$enh -Iruntime -Igen/all -c -o "$od/boot.o" tests/boot.c & pids="$pids $!"
  for p in $pids; do wait "$p" || { echo "derleme hatasi ($od)"; return 1; }; done
}
rt_objs(){ local od="$1" f o=""
  for f in $CSRC $MSRC impargs; do [ -f "$od/$f.o" ] && o="$o $od/$f.o"; done
  echo "$o"; }

if [ "$WHICH" != hd ]; then
  build_rt out/obj 0
  rm -f out/obj/enhance.o            # orijinal build'e girmez
  RT="$(rt_objs out/obj)"
  clang $OPT -Iruntime -Igen/all -c -o out/obj/main.o tests/main_all.c
  clang ${LDOPT} -o out/nfsu2_boot $SHARDS $RT out/obj/boot.o $FW
  clang ${LDOPT} -o out/nfsu2_all  $SHARDS $RT out/obj/main.o $FW
  echo "link OK: orijinal (out/nfsu2_boot)"
fi
if [ "$WHICH" != orig ]; then
  build_rt out/obj_hd 1
  RT="$(rt_objs out/obj_hd)"
  clang ${LDOPT} -o out/nfsu2_hd_boot $SHARDS $RT out/obj_hd/boot.o $FW
  echo "link OK: HD (out/nfsu2_hd_boot)"
fi

# ISA semantik testleri
# Uretec basarisiz olursa SESSIZ GECME.  Eskiden hata yutuluyordu: isatest.py
# haftalarca cokerken eski gen/isatest.c derlenip "439 gecti" diyordu, yani
# testler guncel lifter'i hic sinamiyordu.  Artik bayat ikili silinir ki
# ./run.sh test gorunur sekilde dussun.
if "$ROOT/.venv/bin/python" "$ROOT/tools/isatest.py" >/dev/null \
   && clang -O1 -Wno-unused-label -Iruntime -o out/isatest gen/isatest.c runtime/mem.c -lm; then
  :
else
  rm -f out/isatest
  echo "UYARI: ISA testleri uretilemedi/derlenemedi -- out/isatest silindi" >&2
fi

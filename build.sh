#!/bin/bash
# 23k lift edilmis fonksiyonu paralel derle
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
# -O3 + -mcpu=native olculdu: yaris karesi ortanca 18.7 -> 15.7 ms
# (53.5 -> 63.9 fps).  Lifted kod cok sayida kucuk temel bloktan olusuyor,
# daha agresif inline/sadelestirme dogrudan geri donuyor.
# DAGITIM tabani:
#  -mcpu=apple-m1  : M1 tum Apple Silicon'un ortak paydasi.  "native" ile
#                    derlenirse yeni bir Mac'te uretilen ikili M1'de gecersiz
#                    komutla coker.  M1'de olculen fark sifir.
#  -mmacosx-version-min=11.0 : bayrak verilmezse ikili derleyen makinenin OS
#                    surumuyle damgalanir (minos 26.0) ve eski macOS'ta
#                    "daha yeni surum gerekiyor" diyerek hic acilmaz.
MCPU="${MCPU:-apple-m1}"
MINOS="${MINOS:-11.0}"
# -fno-strict-aliasing ZORUNLU (v2'den itibaren varsayilan): lift kodu misafir
# bellegine ayni adresten farkli tiplerle erisir (MR32 yazip MRF32 okumak);
# bayraksiz clang GVN bunu yanlis derleyebiliyor.  Bkz. LIFTER_VERIMLILIK.md bolum 4.
OPT="${OPT:--O3 -mcpu=$MCPU -mmacosx-version-min=$MINOS -fno-strict-aliasing}"
J="${J:-$(sysctl -n hw.ncpu)}"
mkdir -p "$ROOT/out/obj"
cd "$ROOT/gen/all"
# $OPT'u xargs komut satirina GOMME: -I{} ile kurulan satirin uzunlugu
# sinirli ("command line cannot be assembled, too long").  Disaridan
# ortamla gecir, ic kabuk genisletsin.
#
# Derleyici ciktisini DOGRUDAN "| head -5"e BAGLAMA: head 5 satirdan sonra
# kapaninca clang bir sonraki uyariyi yazarken SIGPIPE ile oluyor ve nesne
# dosyasi HIC yazilmiyordu (shard_0006/0019: 3 denemede 3 kez), ustelik
# build.sh yine 0 donuyordu -> link "symbol(s) not found" ile kiriliyordu.
# Cikti once degiskene alinir, sonra kisaltilir; clang'in cikis kodu korunur
# (herhangi bir shard derlenemezse xargs, dolayisiyla build.sh hata doner).
export OPT ROOT
ls shard_*.c fntab.c stubs.c 2>/dev/null | \
  xargs -P "$J" -n 1 sh -c '
    o=$(clang $OPT -c -I"$ROOT/runtime" -I. -Wno-unused-label -Wno-parentheses-equality \
          -o "$ROOT/out/obj/$(basename "$1" .c).o" "$1" 2>&1); r=$?
    [ -z "$o" ] || printf "%s\n" "$o" | head -5
    [ $r -eq 0 ] || echo "DERLENEMEDI: $1" >&2
    exit $r' sh

#!/bin/bash
# Gercek oyun oturumu kaydi (kullanici oynarken).
#
#   ./tools/oturum.sh            normal surum
#   ./tools/oturum.sh hd         HD surum
#
# Oyun kayit acik (PERFLOG) baslar; normal oyna, bitince oyundan cik.  Ayni anda
# powermetrics CPU/GPU watt'ini saniyede bir yazar (yonetici parolasi bir kez
# sorulur; verilmezse yalniz oyun ici kayit alinir).
# Kayit: ~/nfs_oturum/<tarih>[_hd]   Cozumleme: python3 tools/oturum_analiz.py <dizin>
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HD=0
for a in "$@"; do case "$a" in hd) HD=1 ;; *) echo "bilinmeyen: $a"; exit 1 ;; esac; done
# kayit dizini surumle etiketlenir: hangi surumle oynandigi her zaman belli olsun
SURUM="$(tr -cd '0-9' < "$ROOT/SURUM" 2>/dev/null)"; SURUM="${SURUM:-0}"
AD="NFSU2"; EK="_v$SURUM"; PK=""
[ $HD = 1 ] && { AD="$AD HD"; EK="${EK}_hd"; PK="$PK --hd"; }
AD="$AD.app"
APP="$ROOT/out/kisisel/$AD"; [ -d "$APP" ] || APP="$ROOT/out/$AD"
[ -d "$APP" ] || { echo "uygulama yok: $APP  (once ./pack.sh --embed$PK)"; exit 1; }
DIR="$HOME/nfs_oturum/$(date +%Y%m%d_%H%M%S)$EK"
mkdir -p "$DIR"
echo "uygulama : $APP"
echo "kayit    : $DIR"

PM=0; KA=0
echo "powermetrics icin yonetici parolasi (CPU/GPU watt olcumu):"
if sudo -v; then
  sudo powermetrics --samplers cpu_power,gpu_power,thermal -i 1000 -o "$DIR/guc.txt" >/dev/null 2>&1 &
  PM=1
  ( while sleep 50; do sudo -n -v 2>/dev/null || exit 0; done ) & KA=$!
else
  echo "uyari: parola verilmedi, powermetrics yok; yalniz oyun ici kayit"
fi

# Sicaklik / fan (sudo gerekmez): kullanicinin gordugu "86 derece" dogrudan olculsun
SMC="$ROOT/out/smcoku"; SM=0
[ "$SMC" -nt "$ROOT/tools/smcoku.c" ] || clang -O1 -framework IOKit -o "$SMC" "$ROOT/tools/smcoku.c" 2>/dev/null
if [ -x "$SMC" ]; then "$SMC" -k 1000 "$DIR/sicaklik.csv" & SM=$!; fi

echo
echo "oyun aciliyor: normal oyna (~5 dk), bitince oyundan cik."
# Pil / guc modu: performans dususlerinin bir kismi oyundan degil sistemden geliyor
# (2026-10-18: uykudan %4 pille uyanip sarja takilinca CPU+GPU kisildi).
{ date; pmset -g batt; pmset -g | grep -i lowpowermode; } > "$DIR/pil_bas.txt" 2>&1
# DLINK=2: gercek ekran tazeleme hizini 5 sn'de bir oyunun loguna yazar
open -W -n "$APP" --env PERFLOG="$DIR" --env DLINK=2 --stderr "$DIR/oyun.log"
{ date; pmset -g batt; pmset -g | grep -i lowpowermode; } > "$DIR/pil_son.txt" 2>&1
LG=nfsu2; [ $HD = 1 ] && LG="$LG-hd"
cp "$HOME/Library/Logs/NFSU2/$LG.log" "$DIR/oyun_tam.log" 2>/dev/null

[ $SM != 0 ] && kill $SM 2>/dev/null
if [ $PM = 1 ]; then
  sudo -n pkill -INT -f "powermetrics.*$DIR" 2>/dev/null || sudo pkill -INT -f "powermetrics.*$DIR"
  [ $KA != 0 ] && kill $KA 2>/dev/null
fi
sleep 1
echo
echo "kayit tamam: $DIR ($(du -sh "$DIR" | cut -f1))"
echo "Claude'a bu dizini soyleyebilirsin."

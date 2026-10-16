#!/bin/bash
# Gercek oyun oturumu kaydi (kullanici oynarken).
#
#   ./tools/oturum.sh        normal surum
#   ./tools/oturum.sh hd     HD surum
#
# Oyun kayit acik (PERFLOG) baslar; normal oyna, bitince oyundan cik.  Ayni anda
# powermetrics CPU/GPU watt'ini saniyede bir yazar (yonetici parolasi bir kez
# sorulur; verilmezse yalniz oyun ici kayit alinir).
# Kayit: ~/nfs_oturum/<tarih>[_hd]   Cozumleme: python3 tools/oturum_analiz.py <dizin>
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HD=0; [ "${1:-}" = hd ] && HD=1
if [ $HD = 1 ]; then AD="NFSU2 HD.app"; EK=_hd; else AD="NFSU2.app"; EK=""; fi
APP="$ROOT/out/kisisel/$AD"; [ -d "$APP" ] || APP="$ROOT/out/$AD"
[ -d "$APP" ] || { echo "uygulama yok: $APP  (once ./pack.sh --embed${EK:+ --hd})"; exit 1; }
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

echo
echo "oyun aciliyor: normal oyna (~5 dk), bitince oyundan cik."
open -W -n "$APP" --env PERFLOG="$DIR" --stderr "$DIR/oyun.log"

if [ $PM = 1 ]; then
  sudo -n pkill -INT -f "powermetrics.*$DIR" 2>/dev/null || sudo pkill -INT -f "powermetrics.*$DIR"
  [ $KA != 0 ] && kill $KA 2>/dev/null
fi
sleep 1
echo
echo "kayit tamam: $DIR ($(du -sh "$DIR" | cut -f1))"
echo "Claude'a bu dizini soyleyebilirsin."

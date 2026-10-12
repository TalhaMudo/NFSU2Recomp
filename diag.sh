#!/bin/bash
# Calistir, cokme adresini cikar, cevresini sok, izi goster
ROOT="$(cd "$(dirname "$0")" && pwd)"
# NFSU2_DATA: oyun klasoru (icinde SPEED2.EXE) ya da dogrudan SPEED2.EXE yolu.
# Verilmezse depo kokundeki varsayilan klasor kullanilir.
EXE="$ROOT/Need for Speed Underground 2 - Clean Edition/Need for Speed Underground 2/SPEED2.EXE"
if [ -n "$NFSU2_DATA" ]; then
  if [ -d "$NFSU2_DATA" ]; then EXE="$NFSU2_DATA/SPEED2.EXE"; else EXE="$NFSU2_DATA"; fi
fi
OUT=$(ITRACE=${ITRACE:-0} "$ROOT/out/nfsu2_boot" "$EXE" 2>&1)
echo "$OUT" | grep -E "^  \[COM\]|^com:" | tail -${COMN:-12}
echo "$OUT" | grep -E "toplam import cagrisi|toplam COM cagrisi|Present \(kare\)|Draw\* cagrisi|heap kullanimi|desteklenmeyen komut|cozulemedi"
IP=$(echo "$OUT" | grep -oE "HATAYA NEDEN OLAN GUEST KOMUTU: 0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+")
if [ -z "$IP" ]; then echo "=> COKME YOK"; echo "$OUT" | tail -6; exit 0; fi
echo
echo "=== COKME: $IP ==="
echo "$OUT" | grep -A3 "HATAYA NEDEN" | grep -E "eax=|esp="
echo "$OUT" | grep -A6 "guest yigin izi" | head -7
LO=$(printf "%x" $(( $IP - 0x30 )))
HI=$(printf "%x" $(( $IP + 0x18 )))
"$ROOT/.venv/bin/python" "$ROOT/tools/disasm.py" "$EXE" "$LO-$HI" 2>/dev/null | \
  sed "s/^  $(printf %08x $IP)/>>$(printf %08x $IP)/"

#!/bin/bash
# Run, extract the crash address, disassemble around it, show the trace
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# NFSU2_DATA: the game folder (containing SPEED2.EXE) or the SPEED2.EXE path itself.
# If not given, the default folder at the repository root is used.
EXE="$ROOT/Need for Speed Underground 2 - Clean Edition/Need for Speed Underground 2/SPEED2.EXE"
if [ -n "$NFSU2_DATA" ]; then
  if [ -d "$NFSU2_DATA" ]; then EXE="$NFSU2_DATA/SPEED2.EXE"; else EXE="$NFSU2_DATA"; fi
fi
OUT=$(ITRACE=${ITRACE:-0} "$ROOT/out/nfsu2_boot" "$EXE" 2>&1)
echo "$OUT" | grep -E "^  \[COM\]|^com:" | tail -${COMN:-12}
echo "$OUT" | grep -E "total import calls|total COM calls|Present \(frames\)|Draw\* calls|heap usage|unsupported instructions|unresolved"
IP=$(echo "$OUT" | grep -oE "FAULTING GUEST INSTRUCTION: 0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+")
if [ -z "$IP" ]; then echo "=> NO CRASH"; echo "$OUT" | tail -6; exit 0; fi
echo
echo "=== CRASH: $IP ==="
echo "$OUT" | grep -A3 "FAULTING GUEST" | grep -E "eax=|esp="
echo "$OUT" | grep -A6 "guest stack trace" | head -7
LO=$(printf "%x" $(( $IP - 0x30 )))
HI=$(printf "%x" $(( $IP + 0x18 )))
"$ROOT/.venv/bin/python" "$ROOT/tools/analysis/disasm.py" "$EXE" "$LO-$HI" 2>/dev/null | \
  sed "s/^  $(printf %08x $IP)/>>$(printf %08x $IP)/"

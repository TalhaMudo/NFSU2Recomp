#!/bin/bash
# Install NFSU2 Unlimiter into a game folder on macOS, as Binarius does on Windows.
#
#   tools/mods/ul_install.sh <extracted NFSU2-ULv4.x folder> <game folder>
#
# Runs _NFSU2_Unlimiter-Binarius.endlauncher with tools/mods/binarius (build it
# first: tools/mods/binarius/build.sh).  The game folder IS CHANGED, as Binarius
# changes it: use a copy (on APFS: cp -cR costs no space).
#
# Answers, as the Unlimiter's Read Me recommends: New Installation, Enhanced paint
# groups, Enhanced rim brands, fix wheel textures; no tire mod.  UL_CHOOSE adds or
# overrides answers ("prompt text=option", separated by ";"), e.g.
#   UL_CHOOSE="rim brands=UG2.net"
# The one expected error is SpeedReflect.asi (Binarius' own reflection plugin,
# not used here).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
UL="$(cd "${1:?usage: ul_install.sh <Unlimiter folder> <game folder>}" && pwd)"
GAME="$(cd "${2:?game folder}" && pwd)"
BIN="$ROOT/out/binarius/modinstall/modinstall"
[ -x "$BIN" ] || { echo "build first: tools/mods/binarius/build.sh"; exit 1; }
[ -f "$UL/_NFSU2_Unlimiter-Binarius.endlauncher" ] || { echo "$UL: no _NFSU2_Unlimiter-Binarius.endlauncher"; exit 1; }

# The scripts name files with "\"; on macOS that is a file name character.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/modinstall.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cp -cR "$UL/." "$WORK/" 2>/dev/null || cp -R "$UL/." "$WORK/"
find "$WORK" -type f \( -name '*.endscript' -o -name '*.endlauncher' \) -print0 | xargs -0 perl -pi -e 's#\\#/#g'

# the first answer whose text is in the prompt wins: UL_CHOOSE goes first
args=(--keys "$ROOT/out/binarius/src/Binarius/Binary/Data/MainKeys/underground2.txt")
IFS=';' read -ra extra <<< "${UL_CHOOSE:-}"
for e in "${extra[@]}"; do [ -n "$e" ] && args+=(--choose "$e"); done
args+=(--choose "what to do=New Installation" --choose "paint groups=Enhanced"
       --choose "rim brands=Enhanced" --choose "wheel texture=yes" --choose "tire mod=no")
# Nikki writes MainLog.txt into the working directory: run inside the temporary copy
(cd "$WORK" && "$BIN" "$WORK/_NFSU2_Unlimiter-Binarius.endlauncher" "$GAME" "${args[@]}")

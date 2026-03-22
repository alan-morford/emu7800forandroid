#!/usr/bin/env bash
# copy_core.sh — Copy unmodified emulator core files from emu7800source.
#
# Run from the EMU7800Android/ directory:
#   bash copy_core.sh
#
# The core files are copied verbatim; do NOT modify them.

set -e

SRC="$(dirname "$0")/../emu7800source/plugin/src"
DST="$(dirname "$0")/app/src/main/jni/src/core"

CORE_FILES="
  m6502.c m6502.h
  tia.c   tia.h
  maria.c maria.h
  pia.c   pia.h
  cart.c  cart.h
  machine.c machine.h
  tiasound.c tiasound.h
  pokeysound.c pokeysound.h
  savestate.c savestate.h
"

mkdir -p "$DST"

for f in $CORE_FILES; do
    if [ -f "$SRC/$f" ]; then
        cp "$SRC/$f" "$DST/$f"
        echo "Copied: $f"
    else
        echo "WARNING: $SRC/$f not found — skipping"
    fi
done

echo ""
echo "Done. Core files are in $DST"
echo ""
echo "Next steps:"
echo "  1. Download SDL2-2.30.x and extract into app/src/main/jni/SDL2/"
echo "  2. Copy SDL2/android-project/app/src/main/java/org/ into"
echo "     app/src/main/java/org/  (provides SDLActivity base class)"
echo "  3. Run: ./gradlew assembleDebug"

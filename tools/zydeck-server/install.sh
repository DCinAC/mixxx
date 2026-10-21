#!/usr/bin/env bash
# Puts ZyDeck's server launcher in ~/Documents/Zydek: launch.sh (the stem server and the Discord bot, with the
# pairing QR), qr.py and qrcodegen.py (Project Nayuki, MIT) for the QR, and launch_stem.sh / launch_bot.sh,
# which now hand over to it. The Discord bot's installer runs this too.
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
DEST="$HOME/Documents/Zydek"
mkdir -p "$DEST"
for f in launch.sh launch_stem.sh launch_bot.sh qr.py qrcodegen.py; do
  cp "$SRC/$f" "$DEST/$f"
done
chmod +x "$DEST/launch.sh" "$DEST/launch_stem.sh" "$DEST/launch_bot.sh"
echo "Installed $DEST/launch.sh (./launch.sh: start both and show the pairing QR; see its top for the rest)"

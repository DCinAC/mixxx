#!/bin/sh
# Builds ZyDeck Link for this computer: "ZyDeck Link.app" (zipped) on macOS, a single binary on Linux,
# in dist/. Needs uv (https://docs.astral.sh/uv/); everything else comes with it.
set -e
cd "$(dirname "$0")"
rm -rf build dist mapping
uv run --no-project --with python-rtmidi python zydeck_link.py --write-mapping mapping
if [ "$(uname)" = Darwin ]; then
  uv run --no-project --with python-rtmidi --with pyinstaller pyinstaller --noconfirm --windowed \
    --name "ZyDeck Link" --osx-bundle-identifier org.zydeck.link --add-data mapping:mapping zydeck_link.py
  (cd dist && ditto -c -k --keepParent "ZyDeck Link.app" "ZyDeck-Link-macOS.zip")
else
  uv run --no-project --with python-rtmidi --with pyinstaller pyinstaller --noconfirm --onefile \
    --name zydeck-link --add-data mapping:mapping zydeck_link.py
fi
rm -rf build mapping *.spec
ls -la dist

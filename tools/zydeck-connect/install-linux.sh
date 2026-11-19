#!/usr/bin/env bash
# ZyDeck Connect on Linux: build it here and install it for this user, with a menu entry and its icon, so a phone or
# tablet running ZyDeck in controller mode becomes a MIDI controller for Mixxx on this computer (over the USB cable
# or Wi-Fi). The Mac's own is mac/install-connect.sh; this is its Linux counterpart.
#
#   install-linux.sh              build, install, then start it (and Mixxx after it)
#   install-linux.sh --build      build only (nothing installed, nothing started)
#   install-linux.sh --no-start   install, but don't start anything
#   install-linux.sh --fresh      configure again from scratch (after a big change)
#   install-linux.sh --from DIR   install a prebuilt program from DIR (the CI artifact), not build
#   install-linux.sh -h           this
#
# Run beside a prebuilt zydeck-connect (the CI artifact's folder), it installs that one; in the source tree it builds.
# Building needs Qt 6.4 or newer (Widgets, Network, Sql; Multimedia, when there, decodes tracks for the waveforms),
# ALSA's headers and a C++17 compiler: what's missing it installs with apt (it will ask for your password). The
# program and yt-dlp (for the Web tab's downloads) go to ~/.local/lib/zydeck-connect, a link in ~/.local/bin, and a
# menu entry in ~/.local/share/applications. ZYDECK_CONNECT_PREFIX changes ~/.local; ZYDECK_CONNECT_BUILD the build
# folder. START_MIXXX=0 keeps it from starting Mixxx after Connect.
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"         # this folder: the ZyDeck Connect source
PREFIX="${ZYDECK_CONNECT_PREFIX:-$HOME/.local}"
BUILD="${ZYDECK_CONNECT_BUILD:-$SRC/build}"
APPDIR="$PREFIX/lib/zydeck-connect"          # the program and yt-dlp, together (web.cpp looks beside the program)
BIN="$PREFIX/bin/zydeck-connect"             # a link to it, for the terminal
DESKTOP="$PREFIX/share/applications/zydeck-connect.desktop"
ICONS="$PREFIX/share/icons/hicolor"
# yt-dlp, as CI pins it (the Web tab's downloads need it; without it the rest still works)
YTDLP_VERSION=2026.10.21
YTDLP_SHA256=d8bd33fca9d5b23bd5b9952b3fca36db5c46331933fda9f598a3da14ce2c243f
say() { printf '\033[1;36m%s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m%s\033[0m\n' "$*" >&2; }

# A prebuilt program beside this script (the CI artifact's folder) is installed as it is; the source tree is built
FROM=""
[ -x "$SRC/zydeck-connect" ] && FROM="$SRC"
INSTALL=1; START=1; FRESH=0
while [ $# -gt 0 ]; do
  case $1 in
    --build) INSTALL=0; START=0; FROM="" ;;
    --no-start) START=0 ;;
    --fresh) FRESH=1 ;;
    --from) shift; FROM=${1:?--from needs a folder} ;;
    -h|--help) awk 'NR==1{next} /^#/{print; next} {exit}' "$0"; exit 0 ;;
    *) warn "Unknown option: $1 (--build, --no-start, --fresh, --from DIR)"; exit 2 ;;
  esac
  shift
done

if [ -n "$FROM" ]; then
  PROGRAM="$FROM/zydeck-connect"
  [ -x "$PROGRAM" ] || { warn "No zydeck-connect in $FROM"; exit 1; }
  ICON_PNG="$FROM/zydeck-connect.png"; [ -f "$ICON_PNG" ] || ICON_PNG="$SRC/icon/zydeck-connect.png"
  ICON_SVG="$FROM/zydeck-connect.svg"; [ -f "$ICON_SVG" ] || ICON_SVG="$SRC/icon/zydeck-connect.svg"
  YTDLP_SRC="$FROM/yt-dlp"
  say "Installing the prebuilt ZyDeck Connect from $FROM"
else
  [ -f "$SRC/CMakeLists.txt" ] || { warn "No ZyDeck Connect source at $SRC (and no prebuilt program beside this script)"; exit 1; }
  ICON_PNG="$SRC/icon/zydeck-connect.png"
  ICON_SVG="$SRC/icon/zydeck-connect.svg"
  YTDLP_SRC=""

  # ---- what a build needs -------------------------------------------------------------------------------
  say "Checking the build tools"
  missing=()
  for p in cmake ninja-build g++ qt6-base-dev libgl1-mesa-dev libasound2-dev libxkbcommon-dev; do
    dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
  done
  if [ ${#missing[@]} -gt 0 ]; then
    say "Installing: ${missing[*]}"
    sudo apt-get update -q
    sudo apt-get install -y "${missing[@]}"
  fi
  # Multimedia is optional: without it the waveforms fall back to Mixxx's analysis files
  if ! dpkg -s qt6-multimedia-dev >/dev/null 2>&1; then
    say "Installing qt6-multimedia-dev (the waveforms decode tracks with it)"
    sudo apt-get install -y qt6-multimedia-dev || warn "No qt6-multimedia-dev: the waveforms will be Mixxx's analysis files only"
  fi

  # ---- build --------------------------------------------------------------------------------------------
  # (built from somewhere else before, e.g. the folder moved: CMake won't reuse that build)
  if [ -f "$BUILD/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$BUILD/CMakeCache.txt"; then
    say "The source moved since the last build: configuring again"
    FRESH=1
  fi
  if (( FRESH )); then
    rm -rf "$BUILD"
  fi
  say "Building ZyDeck Connect"
  if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
  fi
  cmake --build "$BUILD"
  PROGRAM="$BUILD/zydeck-connect"
  [ -x "$PROGRAM" ] || { warn "The build made no $PROGRAM"; exit 1; }
  say "Built: $PROGRAM"
fi

if (( ! INSTALL )); then
  say "Nothing installed (--build). Run it with: $PROGRAM"
  exit 0
fi

# ---- install --------------------------------------------------------------------------------------------
say "Installing into $APPDIR"
mkdir -p "$APPDIR" "$PREFIX/bin" "$(dirname "$DESKTOP")" "$ICONS/256x256/apps" "$ICONS/scalable/apps"
install -m 0755 "$PROGRAM" "$APPDIR/zydeck-connect"
[ -f "$ICON_PNG" ] && install -m 0644 "$ICON_PNG" "$ICONS/256x256/apps/zydeck-connect.png"
[ -f "$ICON_SVG" ] && install -m 0644 "$ICON_SVG" "$ICONS/scalable/apps/zydeck-connect.svg"
ln -sf "$APPDIR/zydeck-connect" "$BIN"

if [ -x "$YTDLP_SRC" ]; then
  install -m 0755 "$YTDLP_SRC" "$APPDIR/yt-dlp"
elif [ ! -x "$APPDIR/yt-dlp" ]; then
  say "yt-dlp $YTDLP_VERSION (for the Web tab's downloads)"
  tmp=$(mktemp)
  if curl -fsSL -o "$tmp" "https://github.com/yt-dlp/yt-dlp/releases/download/$YTDLP_VERSION/yt-dlp_linux" &&
     echo "$YTDLP_SHA256  $tmp" | sha256sum -c - >/dev/null 2>&1; then
    install -m 0755 "$tmp" "$APPDIR/yt-dlp"
  else
    warn "Couldn't fetch yt-dlp: the Web tab's downloads won't work (everything else is fine)"
  fi
  rm -f "$tmp"
fi

# The menu entry (the app's own "open at login" writes its autostart entry by itself)
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=ZyDeck Connect
GenericName=ZyDeck controller link
Comment=A phone or tablet running ZyDeck as a MIDI controller for Mixxx
Exec="$APPDIR/zydeck-connect"
Icon=zydeck-connect
Terminal=false
Categories=AudioVideo;Audio;Music;
Keywords=ZyDeck;Mixxx;MIDI;DJ;controller;
StartupNotify=false
EOF
command -v update-desktop-database >/dev/null && update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -f -t "$ICONS" >/dev/null 2>&1 || true
say "Installed: $BIN (and a ZyDeck Connect entry in the menu)"

# ---- start ----------------------------------------------------------------------------------------------
if (( START )); then
  if pgrep -x zydeck-connect >/dev/null; then
    say "ZyDeck Connect is already running"
  else
    say "Starting ZyDeck Connect"
    setsid "$APPDIR/zydeck-connect" >/dev/null 2>&1 < /dev/null &
    for _ in $(seq 1 30); do
      [ "$(curl -s -m 2 -o /dev/null -w '%{http_code}' http://127.0.0.1:8771/profile)" = 200 ] && break
      sleep 1
    done
  fi
  # Mixxx finds Connect's MIDI port only as it starts: start it after Connect (if it isn't already running)
  if [ "${START_MIXXX:-1}" = 1 ] && ! pgrep -ix mixxx >/dev/null; then
    if command -v mixxx >/dev/null; then
      say "Starting Mixxx (it finds Connect's port as it starts)"
      setsid mixxx >/dev/null 2>&1 < /dev/null &
    else
      say "Mixxx isn't on this computer: start it yourself, after Connect, so it finds the port"
    fi
  fi
  say "Pair a device: ZyDeck › Settings › Connections (the cable, or Pair by QR over Wi-Fi)"
fi

#!/usr/bin/env bash
# PortAudio for ZyDeck's Android build: Mixxx's own (its vcpkg overlay: upstream at the pinned commit, with Mixxx's
# patches 0001-0004, 0004 being the Oboe host) plus ours: 0005-ZyDeck-output-presentation-delay.patch adds
# PaOboe_GetOutputPresentationDelay(): when Mixxx's output is heard, from AAudio's timestamps of that very stream (the
# hub draws the waveforms from it); 0006-ZyDeck-output-status-and-queue.patch, PaOboe_GetOutputStatus() (straight to
# the hardware or through Android's mixer, the burst, the queue, AAudio's underruns: each also reaches Mixxx as
# paOutputUnderflow and grows the queue a burst) and PaOboe_SetOutputQueueBursts(). Builds libportaudio.a as Mixxx's
# dependency bundle has it (Oboe, ALSA and JACK hosts, the same exports plus ours) and puts it, with pa_oboe.h, into that
# bundle (the originals kept as *.orig).
# Run on the build PC whenever the bundle is fresh (a new BUILDENV_NAME), before building ZyDeck:
#   bash tools/zydeck-deps/portaudio/build.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HOME/Documents/Zydek/buildenv.env"   # ANDROID_NDK_HOME, BUILDENV_BASEPATH, BUILDENV_NAME
R="$BUILDENV_BASEPATH/$BUILDENV_NAME/installed/arm64-android"
PA_REF=b0cc303e95fdb7c6c953337051378071c9043e88   # as Mixxx's overlay port (mixxxdj/vcpkg overlay/ports/portaudio)
PA_SHA512=fa7b40604ed97c1c5157c4faeb2188e7bbbddd06752811ff8d3facd30151e2f9104737210b7df21cf0f5c0661b28eb6db9b51da09cf015924b4e56835c111c46
MIXXX_VCPKG=https://raw.githubusercontent.com/mixxxdj/vcpkg/2.7/overlay/ports/portaudio
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

curl -fsSL -o src.tar.gz "https://github.com/PortAudio/portaudio/archive/$PA_REF.tar.gz"
echo "$PA_SHA512  src.tar.gz" | sha512sum -c - >/dev/null
tar xzf src.tar.gz && mv "portaudio-$PA_REF" src
for p in 0001-Add-basic-support-for-iOS-to-portaudio.patch 0002-Update-CMakeLists-with-iOS-implementation.patch \
         0003-Fix-renamed-memory-allocation-functions.patch 0004-Add-Android-OBOE.patch; do
  curl -fsSL -o "$p" "$MIXXX_VCPKG/$p"
  (cd src && patch -p1 -s < "../$p")
done
for p in 0005-ZyDeck-output-presentation-delay.patch 0006-ZyDeck-output-status-and-queue.patch; do
  (cd src && patch -p1 -s < "$HERE/$p")
done

PKG_CONFIG_PATH="$R/lib/pkgconfig" PKG_CONFIG_LIBDIR="$R/lib/pkgconfig" cmake -S src -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared -DCMAKE_FIND_ROOT_PATH="$R" \
  -DPA_BUILD_STATIC=ON -DPA_BUILD_SHARED=OFF -DPA_LIBNAME_ADD_SUFFIX=OFF \
  -DOBOE_LIBRARIES="$R/lib/liboboe.a" -DOBOE_INCLUDE_DIR="$R/include/" \
  -DALSA_INCLUDE_DIR="$R/include" -DALSA_LIBRARY="$R/lib/libasound.a" \
  -DPA_USE_JACK=ON -DJACK_INCLUDE_DIR="$R/include" -DJACK_LIBRARY="$R/lib/libjack.a" >/dev/null
cmake --build build >/dev/null

[ -f "$R/lib/libportaudio.a.orig" ] || cp -p "$R/lib/libportaudio.a" "$R/lib/libportaudio.a.orig"
[ -f "$R/include/pa_oboe.h.orig" ] || cp -p "$R/include/pa_oboe.h" "$R/include/pa_oboe.h.orig"
cp build/libportaudio.a "$R/lib/libportaudio.a"
cp src/include/pa_oboe.h "$R/include/pa_oboe.h"
echo "PortAudio with ZyDeck's patch is in $R (the originals: *.orig)"

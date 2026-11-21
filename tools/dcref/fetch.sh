#!/bin/sh
# Fetches what qilin_dcref needs and this repository doesn't hold: the Flycast
# libretro core (GPL; run as a black box, never committed or read into this
# code) and libretro.h (MIT). Into a folder outside the repository, or an
# ignored one.
#   tools/dcref/fetch.sh <folder>
set -eu
dir=${1:?usage: fetch.sh <folder>}
mkdir -p "$dir"
cd "$dir"
curl -fsS -o flycast_libretro.so.zip https://buildbot.libretro.com/nightly/linux/x86_64/latest/flycast_libretro.so.zip
unzip -o -q flycast_libretro.so.zip
rm flycast_libretro.so.zip
curl -fsS -o libretro.h https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h
echo "fetched into $dir: flycast_libretro.so, libretro.h"

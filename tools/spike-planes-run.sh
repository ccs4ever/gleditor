#!/bin/sh
# SPIKE (view project R1/R2/R5), throwaway, not for merge.
# Usage: tools/spike-planes-run.sh <mode> <backend> [extra args]
# Runs headless; vulkan goes through xvfb-run because SDL2's offscreen driver
# cannot make a Vulkan window.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
mode=$1
backend=$2
shift 2
mkdir -p "$root/spike-out"
export SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
export XDG_DATA_HOME="$root/build/xdg/data"
export XDG_CONFIG_HOME="$root/build/xdg/config"
export XDG_CACHE_HOME="$root/build/xdg/cache"
if [ "$backend" = vulkan ]; then
  export SDL_VIDEODRIVER=x11
  export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
  exec xvfb-run -a -s "-screen 0 1280x800x24" \
    "$root/build/spike-planes" "$mode" --backend "$backend" "$@"
fi
export SDL_VIDEODRIVER=offscreen
exec "$root/build/spike-planes" "$mode" --backend "$backend" "$@"

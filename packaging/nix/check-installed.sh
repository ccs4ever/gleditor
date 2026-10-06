#!/bin/sh
# Validate installed Nix binaries using a matching software graphics/AT-SPI closure.
set -eu
evidence=${GLEDITOR_A11Y_EVIDENCE:-}
unset GLEDITOR_A11Y_EVIDENCE
package=${1:?package path required}
smoke=${2:?smoke helper required}
native=${3:?native helper required}
mesa=${4:?Mesa path required}
shift 4
mode=${1:-}

# Mesa names the Lavapipe ICD for the target architecture.
for icd in "$mesa"/share/vulkan/icd.d/lvp_icd*.json; do
  [ -f "$icd" ] || continue
  export VK_DRIVER_FILES="$icd"
  break
done
: "${VK_DRIVER_FILES:?Lavapipe ICD is missing}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export XDG_DATA_HOME="$work/data"
export XDG_CONFIG_HOME="$work/config"
export XDG_CACHE_HOME="$work/cache"
export XDG_RUNTIME_DIR="$work/runtime"
mkdir -m 700 "$XDG_RUNTIME_DIR"
if [ "$mode" = --navigation ]; then
  if [ -n "$evidence" ]; then
    export GLEDITOR_A11Y_EVIDENCE="$evidence"
  fi
  python3 "$native" "$package/bin/xuzz" --navigation
  exit 0
elif [ -n "$mode" ]; then
  echo "Unknown validation mode: $mode" >&2
  exit 2
fi

# The virtual display handles Vulkan presentation without touching a user's screen.
unset SDL_VIDEODRIVER
for program in gleditor xuzz; do
  xvfb-run -a sh "$smoke" "$package/bin/$program" opengl vulkan
  python3 "$native" "$package/bin/$program"
done

# Focused native Walks metadata and note controls, through the external client.
if [ -n "$evidence" ]; then
  export GLEDITOR_A11Y_EVIDENCE="$evidence"
fi
python3 "$native" "$package/bin/xuzz" --walks

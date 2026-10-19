#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
prototype_root="${KJV_PROTOTYPE_ROOT:-$PWD/build/kjv-link-prototype}"
if [[ ! -f "$prototype_root/prototype.json" ]]; then
  echo 'Prepare the prototype first with the prototype/kjv-links make target.' >&2
  exit 1
fi
export XDG_DATA_HOME="$prototype_root/data"
export XDG_CONFIG_HOME="$prototype_root/config"
export XDG_CACHE_HOME="$prototype_root/cache"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
mapfile -t first_passage < <(
  python3 - "$prototype_root/prototype.json" <<'PY'
import json
import sys
with open(sys.argv[1], encoding='utf-8') as source:
    first = json.load(source)['left'][0]
print(first['start'])
print(first['start'] + first['length'])
PY
)
exec ./build/xuzz "$prototype_root/kjv" --view xanadoc --no-sworph \
  --chord Ctrl+O --chord Tab --type "$prototype_root/notes" --chord Return \
  --chord Ctrl+1 --select "${first_passage[0]},${first_passage[1]}" \
  --chord Alt+Shift+N --chord Alt+Shift+J --chord Alt+Shift+L \
  --chord Alt+Shift+X --chord Alt+Shift+L --chord Alt+Shift+X \
  --chord Alt+Shift+V --chord Alt+Shift+Return "$@"

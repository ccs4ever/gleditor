#!/bin/sh
set -eu

# Every distribution uses the same binding and verifies the release archive.
destination=${1:?usage: fetch.sh <destination-directory>}
version=0.22.3
checksum=b652e380fb78efe6721ad892f15b2224f38f661c3fb20436ef4c5b3ce0fe8177
mkdir -p "$destination"
archive="$destination/accesskit-c-$version.zip"
if [ ! -f "$archive" ]; then
  curl -fL --retry 3 -o "$archive.tmp" \
    "https://github.com/AccessKit/accesskit-c/releases/download/$version/accesskit-c-$version.zip"
  mv "$archive.tmp" "$archive"
fi
if command -v sha256sum >/dev/null 2>&1; then
  actual=$(sha256sum "$archive" | cut -d ' ' -f 1)
else
  actual=$(shasum -a 256 "$archive" | cut -d ' ' -f 1)
fi
if [ "$actual" != "$checksum" ]; then
  echo "AccessKit release checksum mismatch: $archive" >&2
  exit 1
fi
unzip -oq "$archive" -d "$destination"

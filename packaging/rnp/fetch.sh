#!/usr/bin/env bash
set -euo pipefail

destination=${1:?usage: fetch.sh <dependency directory>}
mkdir -p "$destination"
destination=$(cd "$destination" && pwd)
archive="$destination/rnp-v0.18.1.tar.gz"
if [ ! -f "$archive" ]; then
  curl -fL --retry 3 \
    https://github.com/rnpgp/rnp/releases/download/v0.18.1/rnp-v0.18.1.tar.gz \
    -o "$archive.tmp"
  mv "$archive.tmp" "$archive"
fi
checksum=423c8e32e1e591462f759adf8441b1c44bca96d9f5daff13b82e81a79f18ecfd
echo "$checksum  $archive" | sha256sum -c -
tar -xf "$archive" -C "$destination"

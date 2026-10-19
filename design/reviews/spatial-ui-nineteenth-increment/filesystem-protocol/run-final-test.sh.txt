#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
run_root=$(mktemp -d "$PWD/build/validation-nineteenth/xdg-$1.XXXXXX")
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1
export XDG_DATA_HOME="$run_root/data" XDG_CONFIG_HOME="$run_root/config" XDG_CACHE_HOME="$run_root/cache"
case "$1" in
 ui) exec ./build/zigzag_test ;;
 xuzz) exec ./build/xuzz_test ;;
 owning) exec ./build/xudu_test --gtest_filter='PublicationOutboxTest.*:PublicationInboxTest.*:PublicationSubscriptionsTest.*:ReaderLinkPackagesTest.*:LinkPackageExchangeTest.*' ;;
 *) exit 2 ;;
esac

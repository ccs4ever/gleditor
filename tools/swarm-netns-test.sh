#!/bin/sh
# Run the swarm tests with the two peers on separate network stacks.
#
# The point of the arrangement is that a transfer which succeeds has to have
# gone over a network device. Both peers get a network namespace of their own,
# so they have separate addresses, separate routing and -- the part that
# matters -- separate loopbacks: neither can reach the other by accident, and
# 127.0.0.1 means something different to each of them. They are joined by a
# veth pair and nothing else. There is no DHT, no local discovery and no
# tracker, so the swarm contains exactly the two peers that were introduced.
#
# Needs root, because creating a network namespace does. It is not part of
# `make test` for that reason: see the note in README.md about what runs where.
#
# Usage: tools/swarm-netns-test.sh [sample-file]

set -e

SAMPLE=${1:-tests/samples/quick_brown_fox.txt}
PEER_NS=xudu-seed
TEST_NS=xudu-leech
PEER_IP=10.77.0.1
TEST_IP=10.77.0.2
WORK=$(mktemp -d)
PEER_PID=

# Reached only via the trap below, which shellcheck cannot see -- everything
# in here is live code, not dead.
# shellcheck disable=SC2317
cleanup() {
  if [ -n "$PEER_PID" ]; then
    kill "$PEER_PID" 2>/dev/null || true
  fi
  ip netns del "$PEER_NS" 2>/dev/null || true
  ip netns del "$TEST_NS" 2>/dev/null || true
  rm -rf "$WORK"
}
if [ "$(id -u)" != 0 ]; then
  if command -v unshare >/dev/null 2>&1 && unshare -Urnm true 2>/dev/null; then
    exec unshare -Urnm sh -c '
      mount -t tmpfs tmpfs /var/run 2>/dev/null || true
      mkdir -p /var/run/netns 2>/dev/null || true
      exec "$@"
    ' -- "$0" "$@"
  fi
  echo "swarm-netns-test: needs root or unprivileged user namespaces (unshare -Urnm) to create network namespaces (skipping)" >&2
  exit 0
fi
trap cleanup EXIT INT TERM
# A one-item list on purpose, so a second required tool is one more word
# rather than a second copy of this loop.
# shellcheck disable=SC2043
for tool in ip; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "swarm-netns-test: $tool is not installed (iproute2), skipping swarm network test" >&2
    exit 0
  }
done
[ -x build/xudu-swarm-peer ] || {
  echo "swarm-netns-test: build/xudu-swarm-peer is missing; run 'make xudu-swarm-peer'" >&2
  exit 1
}
[ -x build/xudu_test ] || {
  echo "swarm-netns-test: build/xudu_test is missing; run 'make xudu_test'" >&2
  exit 1
}

echo "==> making a torrent of $SAMPLE"
mkdir -p "$WORK/seed"
cp "$SAMPLE" "$WORK/seed/sample.txt"
# Built here rather than checked in, so the test covers whatever sample it is
# pointed at. A small piece length so that even a short file has several
# pieces, which is what makes the piece-spanning read meaningful.
python3 - "$WORK/seed/sample.txt" "$WORK/sample.torrent" <<'PY'
import hashlib, sys
def ben(v):
    if isinstance(v, int):   return b"i%de" % v
    if isinstance(v, bytes): return b"%d:%s" % (len(v), v)
    if isinstance(v, list):  return b"l" + b"".join(ben(x) for x in v) + b"e"
    if isinstance(v, dict):  return b"d" + b"".join(ben(k) + ben(v[k]) for k in sorted(v)) + b"e"
data = open(sys.argv[1], "rb").read()
plen = 64
pieces = b"".join(hashlib.sha1(data[i:i + plen]).digest() for i in range(0, len(data), plen))
info = {b"name": b"sample.txt", b"piece length": plen, b"pieces": pieces, b"length": len(data)}
open(sys.argv[2], "wb").write(ben({b"info": info}))
print("  %d bytes, %d pieces, info hash %s" % (len(data), len(pieces) // 20,
                                               hashlib.sha1(ben(info)).hexdigest()))
PY

echo "==> two network namespaces, joined by a veth pair"
ip netns del "$PEER_NS" 2>/dev/null || true
ip netns del "$TEST_NS" 2>/dev/null || true
ip netns add "$PEER_NS"
ip netns add "$TEST_NS"
ip link add xudu-s type veth peer name xudu-l
ip link set xudu-s netns "$PEER_NS"
ip link set xudu-l netns "$TEST_NS"
ip netns exec "$PEER_NS" sh -c "ip addr add $PEER_IP/24 dev xudu-s; ip link set xudu-s up; ip link set lo up"
ip netns exec "$TEST_NS" sh -c "ip addr add $TEST_IP/24 dev xudu-l; ip link set xudu-l up; ip link set lo up"
echo "  seeder $PEER_IP, leecher $TEST_IP"

echo "==> starting the seeder in $PEER_NS"
# --publish also points a fresh ed25519 key at the torrent as a BEP 46 mutable
# item, so the leecher can be given a name rather than a hash and made to go
# and look up what it currently means.
ip netns exec "$PEER_NS" ./build/xudu-swarm-peer \
  "$WORK/sample.torrent" "$WORK/seed" "$PEER_IP" --publish \
  >"$WORK/peer.out" 2>"$WORK/peer.err" &
PEER_PID=$!

# Wait for it to say which port it took, rather than guessing how long it needs.
PEER_PORT=
i=0
while [ "$i" -lt 100 ]; do
  PEER_PORT=$(sed -n 's/^port //p' "$WORK/peer.out" 2>/dev/null | head -1)
  [ -n "$PEER_PORT" ] && break
  kill -0 "$PEER_PID" 2>/dev/null || {
    echo "seeder died:"
    cat "$WORK/peer.err"
    exit 1
  }
  i=$((i + 1))
  sleep 0.1
done
[ -n "$PEER_PORT" ] || {
  echo "seeder never reported a port:"
  cat "$WORK/peer.err"
  exit 1
}
echo "  listening on $PEER_IP:$PEER_PORT"

# Printed after the port, so by now it is there.
PEER_PUBKEY=$(sed -n 's/^pubkey //p' "$WORK/peer.out" | head -1)
[ -n "$PEER_PUBKEY" ] || {
  echo "seeder never reported a public key:"
  cat "$WORK/peer.err"
  exit 1
}
echo "  publishing as $PEER_PUBKEY"

echo "==> running the swarm tests in $TEST_NS"
set +e
ip netns exec "$TEST_NS" env \
  XUDU_PEER_HOST="$PEER_IP" \
  XUDU_PEER_PORT="$PEER_PORT" \
  XUDU_PEER_TORRENT="$WORK/sample.torrent" \
  XUDU_PEER_TEXT="$WORK/seed/sample.txt" \
  XUDU_PEER_PUBKEY="$PEER_PUBKEY" \
  LD_LIBRARY_PATH="$PWD/build" \
  ./build/xudu_test --gtest_filter='SwarmTest.*:MutableNameTest.*'
STATUS=$?
set -e

if [ "$STATUS" != 0 ]; then
  echo "--- seeder stderr ---"
  cat "$WORK/peer.err"
fi
exit "$STATUS"

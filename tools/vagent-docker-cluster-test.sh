#!/usr/bin/env bash
#
# tools/vagent-docker-cluster-test.sh
#
# Slow, integration, end-to-end 3-node vagent cluster test.
#
# Stands up 3 isolated container nodes on a private Docker bridge network:
#   - Node 1 (Producer / Sensor):
#       Packages cluster agent with `vagent package`, approves authority in
#       ledger, and executes SensorProducer to emit `SensorBatch`.
#   - Node 2 (Compute Worker):
#       Transfers package from Node 1 over the Docker bridge network, inspects
#       integrity with `vagent package inspect`, and executes ComputeWorker to
#       update epistemic beliefs and emit `ComputeResult`.
#   - Node 3 (Cluster Auditor):
#       Transfers compute results from Node 2 and package from Node 1, inspects
#       package, and executes ClusterAuditor to assert epistemic invariants across
#       all 3 nodes and emit `AuditVerdict { ... status: "VERIFIED" }`.
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="${ROOT}/build/vagent"

# Pre-flight check: Docker binary and active daemon
if ! command -v docker >/dev/null 2>&1; then
  if [ "${REQUIRE_DOCKER:-0}" = "1" ]; then
    echo "==> [ERROR] docker command not found but REQUIRE_DOCKER=1" >&2
    exit 1
  fi
  echo "==> [SKIP] Docker binary not found; skipping 3-node vagent cluster test."
  exit 0
fi

if ! docker info >/dev/null 2>&1; then
  if [ "${REQUIRE_DOCKER:-0}" = "1" ]; then
    echo "==> [ERROR] Docker daemon is not running or accessible but REQUIRE_DOCKER=1" >&2
    exit 1
  fi
  echo "==> [SKIP] Docker daemon not running or current user lacks access; skipping 3-node vagent cluster test."
  exit 0
fi

# Ensure vagent binary is built
if [ ! -x "$BIN" ]; then
  echo "==> Building vagent binary..."
  make -C "$ROOT" -j"$(nproc)" vagent
fi

IMAGE="archlinux:base"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "==> Pulling Docker base image: ${IMAGE}..."
  docker pull "$IMAGE"
fi

ID="$$"
NET="vagent-cluster-net-${ID}"
NODE1="vagent-node-1-${ID}"
NODE2="vagent-node-2-${ID}"
NODE3="vagent-node-3-${ID}"
USER_FLAGS=(
  --user "$(id -u):$(id -g)"
  -e "HOME=/work"
  -e "XDG_DATA_HOME=/work/xdg/data"
  -e "XDG_CONFIG_HOME=/work/xdg/config"
  -e "XDG_CACHE_HOME=/work/xdg/cache"
  -e "SDL_VIDEODRIVER=offscreen"
  -e "SDL_AUDIODRIVER=dummy"
  -e "LIBGL_ALWAYS_SOFTWARE=1"
)

WORK=$(mktemp -d /tmp/vagent-cluster-test.XXXXXX)
mkdir -p "${WORK}/node1" "${WORK}/node2" "${WORK}/node3"

cleanup() {
  trap - EXIT INT TERM
  echo "==> Cleaning up Docker containers and network..."
  docker rm -f "$NODE1" "$NODE2" "$NODE3" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

echo "==> [1/7] Creating internal Docker bridge network: ${NET}..."
docker network create --internal "$NET"

echo "==> [2/7] Writing multi-agent pipeline specification..."
cat <<'EOF' >"${WORK}/node1/pipeline.vagent"
predicate reading(batch_id: Str, sensor: Str, count: Int, raw: Int)
predicate computed(batch_id: Str, score: Int)
predicate audited(batch_id: Str, status: Str)

event StartSensor { batch_id: Str, readings: Int, raw: Int }
event SensorBatch { batch_id: Str, sensor_id: Str, readings_count: Int, total_raw: Int }
event ComputeResult { batch_id: Str, node_id: Str, total_raw: Int, scaled_score: Int }
event AuditVerdict { batch_id: Str, nodes_verified: Int, total_score: Int, status: Str }

agent SensorProducer {
  on StartSensor as s {
    emit SensorBatch{batch_id: s.batch_id, sensor_id: "node-1-sensor", readings_count: s.readings, total_raw: s.raw} in outer
  }
}

agent ComputeWorker {
  on SensorBatch as b {
    believe reading(b.batch_id, b.sensor_id, b.readings_count, b.total_raw)
    let score := b.total_raw * 2
    believe computed(b.batch_id, score)
    emit ComputeResult{batch_id: b.batch_id, node_id: "node-2-compute", total_raw: b.total_raw, scaled_score: score} in outer
  }
}

agent ClusterAuditor {
  on ComputeResult as c {
    let ok := c.scaled_score >= 100
    if ok then {
      believe audited(c.batch_id, "VERIFIED")
      emit AuditVerdict{batch_id: c.batch_id, nodes_verified: 3, total_score: c.scaled_score, status: "VERIFIED"} in outer
    }
  }
}
EOF

echo "==> [3/7] Standing up 3 container nodes on ${NET}..."
docker run -d --name "$NODE1" --hostname node-1 --network "$NET" \
  "${USER_FLAGS[@]}" \
  -v "${ROOT}":/opt/gleditor:ro \
  -v /usr:/usr:ro \
  -v "${WORK}/node1":/work:rw \
  "$IMAGE" sleep 300 >/dev/null

docker run -d --name "$NODE2" --hostname node-2 --network "$NET" \
  "${USER_FLAGS[@]}" \
  -v "${ROOT}":/opt/gleditor:ro \
  -v /usr:/usr:ro \
  -v "${WORK}/node2":/work:rw \
  "$IMAGE" sleep 300 >/dev/null

docker run -d --name "$NODE3" --hostname node-3 --network "$NET" \
  "${USER_FLAGS[@]}" \
  -v "${ROOT}":/opt/gleditor:ro \
  -v /usr:/usr:ro \
  -v "${WORK}/node3":/work:rw \
  "$IMAGE" sleep 300 >/dev/null

echo "==> [4/7] Node 1 (Producer): Packaging pipeline, approving authority..."
docker exec "$NODE1" /opt/gleditor/build/vagent package /work/pipeline.vagent \
  -o /work/pipeline.vnpkg --author node-1-producer --device dev-1 >/dev/null

docker exec "$NODE1" mkdir -p /work/authorities
docker exec "$NODE1" /opt/gleditor/build/vagent native approve /work/pipeline.vnpkg \
  --kind compute.scale --ledger /work/authorities --as "node-1-producer" >/dev/null

# Distribute signed package to node volumes for inspection
cp "${WORK}/node1/pipeline.vnpkg" "${WORK}/node2/pipeline.vnpkg"
cp "${WORK}/node1/pipeline.vnpkg" "${WORK}/node3/pipeline.vnpkg"

echo "==> [5/7] Node 2 & Node 3: Inspecting package integrity across cluster..."
PKG_INSPECT_NODE2=$(docker exec "$NODE2" /opt/gleditor/build/vagent package inspect /work/pipeline.vnpkg)
echo "$PKG_INSPECT_NODE2" | grep -q "Package Format:    vagent-package-1"
echo "$PKG_INSPECT_NODE2" | grep -q "Author:            node-1-producer"

PKG_INSPECT_NODE3=$(docker exec "$NODE3" /opt/gleditor/build/vagent package inspect /work/pipeline.vnpkg)
echo "$PKG_INSPECT_NODE3" | grep -q "Package Format:    vagent-package-1"

echo "==> [6/7] Starting concurrent cluster nodes (Node 3, Node 2, Node 1) with TCP internode routing..."

# Node 3 (Auditor): listens on 9003, awaits ComputeResult, emits AuditVerdict
docker exec "$NODE3" /opt/gleditor/build/vagent node \
  --node node-3 \
  --listen 9003 \
  --listen-host 0.0.0.0 \
  --program /work/pipeline.vnpkg \
  --agent ClusterAuditor \
  --exit-on AuditVerdict \
  --timeout 25s >"${WORK}/node3/node3.log" 2>&1 &
PID_NODE3=$!

# Node 2 (Compute Worker): listens on 9002, peers with node-3, forwards ComputeResult -> node-3
docker exec "$NODE2" /opt/gleditor/build/vagent node \
  --node node-2 \
  --listen 9002 \
  --listen-host 0.0.0.0 \
  --peer "node-3=${NODE3}:9003" \
  --forward "ComputeResult=node-3:ClusterAuditor" \
  --program /work/pipeline.vnpkg \
  --agent ComputeWorker \
  --exit-on ComputeResult \
  --timeout 25s >"${WORK}/node2/node2.log" 2>&1 &
PID_NODE2=$!

# Allow listening sockets to initialize
sleep 0.5

# Node 1 (Sensor): listens on 9001, peers with node-2, forwards SensorBatch -> node-2, delivers StartSensor
docker exec "$NODE1" /opt/gleditor/build/vagent node \
  --node node-1 \
  --listen 9001 \
  --listen-host 0.0.0.0 \
  --peer "node-2=${NODE2}:9002" \
  --forward "SensorBatch=node-2:ComputeWorker" \
  --program /work/pipeline.vnpkg \
  --agent SensorProducer \
  --send 'StartSensor{batch_id: "batch-4242", readings: 12, raw: 85}' \
  --exit-on SensorBatch \
  --timeout 25s >"${WORK}/node1/node1.log" 2>&1

# Wait for concurrent worker and auditor nodes to finish their routed turns
wait "$PID_NODE2"
wait "$PID_NODE3"

echo "==> [7/7] Verifying 3-node cluster logs and internode event routing..."

echo "--- Node 1 Emission Log ---"
cat "${WORK}/node1/node1.log"
echo "--- Node 2 Emission Log ---"
cat "${WORK}/node2/node2.log"
echo "--- Node 3 Emission Log ---"
cat "${WORK}/node3/node3.log"

FINAL_REPORT=$(cat "${WORK}/node3/node3.log")

if ! grep -q 'SensorBatch' "${WORK}/node1/node1.log"; then
  echo "==> [FAIL] Expected SensorBatch emission from Node 1" >&2
  exit 1
fi

if ! grep -q 'ComputeResult' "${WORK}/node2/node2.log"; then
  echo "==> [FAIL] Expected ComputeResult emission from Node 2" >&2
  exit 1
fi

if ! echo "$FINAL_REPORT" | grep -q 'AuditVerdict'; then
  echo "==> [FAIL] Expected AuditVerdict emission from Node 3" >&2
  exit 1
fi

if ! echo "$FINAL_REPORT" | grep -q 'batch-4242'; then
  echo "==> [FAIL] Missing batch_id batch-4242 in Node 3 report" >&2
  exit 1
fi

if ! echo "$FINAL_REPORT" | grep -q 'nodes_verified: 3'; then
  echo "==> [FAIL] Expected 3 verified nodes in Node 3 report" >&2
  exit 1
fi

if ! echo "$FINAL_REPORT" | grep -q 'status: "VERIFIED"'; then
  echo "==> [FAIL] Missing VERIFIED status in Node 3 report" >&2
  exit 1
fi

if ! echo "$FINAL_REPORT" | grep -q 'total_score: 170'; then
  echo "==> [FAIL] Expected total_score 170 (85 * 2) in Node 3 report" >&2
  exit 1
fi

echo "==> 3-node concurrent Docker cluster test PASSED with live internode TCP routing across ${NODE1}, ${NODE2}, ${NODE3}!"

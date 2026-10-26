/**
 * @file or_parallel.cpp
 * @brief Implementation of Adaptive Oracle (Model 1) + Shadow-Delta (Model 2)
 *        OR-Parallel Search Coordinator.
 */
#include "common/xanadu/vortex/or_parallel.hpp"

#include <algorithm>
#include <ranges>

#include "common/xanadu/zigzag/cell_views.hpp"
#include <gleditor/sentinel.hpp>

namespace zigzag::vortex {

namespace {

void writeU8(std::string &out, const std::uint8_t v) {
  out.push_back(static_cast<char>(v));
}

void writeU16(std::string &out, const std::uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFFU));
  out.push_back(static_cast<char>((v >> 8U) & 0xFFU));
}

void writeU32(std::string &out, const std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xFFU));
  }
}

void writeU64(std::string &out, const std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xFFULL));
  }
}

void writeStr(std::string &out, const std::string_view s) {
  writeU32(out, static_cast<std::uint32_t>(s.size()));
  out.append(s.data(), s.size());
}

bool readU8(std::string_view &in, std::uint8_t &out) {
  if (in.empty()) return false;
  out = static_cast<std::uint8_t>(in.front());
  in.remove_prefix(1);
  return true;
}

bool readU16(std::string_view &in, std::uint16_t &out) {
  if (in.size() < 2) return false;
  out = static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[0])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[1])) << 8U);
  in.remove_prefix(2);
  return true;
}

bool readU32(std::string_view &in, std::uint32_t &out) {
  if (in.size() < 4) return false;
  out = 0;
  for (int i = 0; i < 4; ++i) {
    out |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i]))
           << (i * 8);
  }
  in.remove_prefix(4);
  return true;
}

bool readU64(std::string_view &in, std::uint64_t &out) {
  if (in.size() < 8) return false;
  out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[i]))
           << (i * 8);
  }
  in.remove_prefix(8);
  return true;
}

bool readStr(std::string_view &in, std::string &out) {
  std::uint32_t len = 0;
  if (!readU32(in, len) || in.size() < len) return false;
  out.assign(in.data(), len);
  in.remove_prefix(len);
  return true;
}

} // namespace

std::string encodeOrParallelSpark(const OrParallelSpark &spark) {
  std::string out;
  out.append("ORSP", 4);
  writeU8(out, static_cast<std::uint8_t>(spark.model));
  writeU32(out, static_cast<std::uint32_t>(spark.deweyPath.size()));
  for (const auto step : spark.deweyPath) {
    writeU16(out, step);
  }
  writeStr(out, spark.deweyKey);
  writeU64(out, spark.cyclesSinceRoot);
  writeU32(out, spark.entryOpcode);
  writeU32(out, spark.cursor.value_or(noCell));
  writeU32(out, spark.stackFrame.value_or(noCell));
  if (spark.prefixSnapshot.has_value()) {
    writeU8(out, 1);
    writeStr(out, encodeArenaPrefix(*spark.prefixSnapshot));
  } else {
    writeU8(out, 0);
  }
  return out;
}

std::optional<OrParallelSpark> decodeOrParallelSpark(std::string_view bytes) {
  if (bytes.size() < 4 || bytes.substr(0, 4) != "ORSP") {
    return std::nullopt;
  }
  bytes.remove_prefix(4);

  OrParallelSpark out{};
  std::uint8_t modelRaw = 0;
  std::uint32_t pathLen = 0;
  if (!readU8(bytes, modelRaw) || !readU32(bytes, pathLen) ||
      pathLen > 65535U) {
    return std::nullopt;
  }
  out.model = (modelRaw == 0) ? SparkModelKind::OracleRecompute
                              : SparkModelKind::ShadowDeltaPrefix;
  out.deweyPath.resize(pathLen);
  for (std::uint32_t i = 0; i < pathLen; ++i) {
    if (!readU16(bytes, out.deweyPath[i])) {
      return std::nullopt;
    }
  }
  std::uint32_t stackFrame = noCell;
  std::uint32_t cursor     = noCell;
  if (!readStr(bytes, out.deweyKey) || !readU64(bytes, out.cyclesSinceRoot) ||
      !readU32(bytes, out.entryOpcode) || !readU32(bytes, cursor) ||
      !readU32(bytes, stackFrame)) {
    return std::nullopt;
  }
  out.cursor             = gleditor::fromSentinel<noCell>(cursor);
  out.stackFrame         = gleditor::fromSentinel<noCell>(stackFrame);
  std::uint8_t hasPrefix = 0;
  if (!readU8(bytes, hasPrefix)) {
    return std::nullopt;
  }
  if (hasPrefix != 0) {
    std::string prefixBytes;
    if (!readStr(bytes, prefixBytes)) {
      return std::nullopt;
    }
    auto decoded = decodeArenaPrefix(prefixBytes);
    if (!decoded.has_value()) {
      return std::nullopt;
    }
    out.prefixSnapshot = std::move(*decoded);
  }
  return out;
}

bool isDeweySubtreeOf(const std::string_view candidateDeweyKey,
                      const std::string_view prunedPrefix) noexcept {
  if (prunedPrefix.empty()) {
    return true;
  }
  if (candidateDeweyKey == prunedPrefix) {
    return true;
  }
  if (candidateDeweyKey.size() > prunedPrefix.size() &&
      candidateDeweyKey.starts_with(prunedPrefix) &&
      candidateDeweyKey[prunedPrefix.size()] == '.') {
    return true;
  }
  return false;
}

OrParallelCoordinator::OrParallelCoordinator(const Manifold *base,
                                             std::uint64_t cycleThreshold)
    : cycleThreshold_(cycleThreshold), arena_(base), core_(arena_),
      dimOr_(arena_.ensureDimension("d.or")),
      dimBranch_(arena_.ensureDimension("d.branch")),
      dimFrontier_(arena_.ensureDimension("d.frontier")),
      dimSubmission_(arena_.ensureDimension("d.submission")) {
  goalRoot_ = arena_.makeCell("or_goal_root");
  zigzag::expectWritten(arena_.link(core_.home(), dimOr_, false, goalRoot_));
}

std::optional<OrParallelSpark>
OrParallelCoordinator::exportSparkFromVictim(VortexVM &victimVm,
                                             const CellRef rootEntryOpcode) {
  auto stolen = victimVm.stealNearRootChoicePoint();
  if (!stolen.has_value()) {
    return std::nullopt;
  }

  OrParallelSpark spark{};
  spark.deweyPath       = std::move(stolen->deweyPath);
  spark.deweyKey        = formatDeweyPath(spark.deweyPath);
  spark.cyclesSinceRoot = stolen->cyclesSinceRoot;
  spark.cursor          = stolen->cursor;
  spark.stackFrame      = stolen->stackFrame;

  if (stolen->cyclesSinceRoot < cycleThreshold_) {
    spark.model       = SparkModelKind::OracleRecompute;
    spark.entryOpcode = rootEntryOpcode;
  } else {
    spark.model          = SparkModelKind::ShadowDeltaPrefix;
    spark.entryOpcode    = stolen->altOp;
    spark.prefixSnapshot = victimVm.core().arena().exportPrefix(stolen->mark);
  }

  enqueueSpark(spark);
  return spark;
}

OrParallelCoordinator *
OrParallelCoordinator::enqueueSpark(OrParallelSpark spark) {
  std::scoped_lock lock(mu_);
  if (spark.deweyKey.empty() && !spark.deweyPath.empty()) {
    spark.deweyKey = formatDeweyPath(spark.deweyPath);
  }
  for (const auto &prefix : prunedPrefixes_) {
    if (isDeweySubtreeOf(spark.deweyKey, prefix)) {
      return this;
    }
  }
  const CellRef branchCell = arena_.makeCell(spark.deweyKey);
  zigzag::expectWritten(
      arena_.link(zigzag::rankTail(arena_, goalRoot_, dimBranch_), dimBranch_,
                  false, branchCell));
  frontierQueue_.push_back(std::move(spark));
  rebuildFrontierRankLocked();
  return this;
}

std::optional<std::pair<OrParallelSpark, std::stop_token>>
OrParallelCoordinator::acquireNextSpark() {
  std::scoped_lock lock(mu_);
  while (!frontierQueue_.empty()) {
    OrParallelSpark next = std::move(frontierQueue_.front());
    frontierQueue_.erase(frontierQueue_.begin());
    bool pruned = false;
    for (const auto &prefix : prunedPrefixes_) {
      if (isDeweySubtreeOf(next.deweyKey, prefix)) {
        pruned = true;
        break;
      }
    }
    if (pruned) {
      continue;
    }
    rebuildFrontierRankLocked();
    auto &source = activeStopSources_[next.deweyKey];
    return std::make_pair(std::move(next), source.get_token());
  }
  rebuildFrontierRankLocked();
  return std::nullopt;
}

std::stop_token
OrParallelCoordinator::registerActiveSpark(const std::string_view deweyKey) {
  std::scoped_lock lock(mu_);
  auto &source = activeStopSources_[std::string(deweyKey)];
  for (const auto &prefix : prunedPrefixes_) {
    if (isDeweySubtreeOf(deweyKey, prefix)) {
      source.request_stop();
      break;
    }
  }
  return source.get_token();
}

std::size_t
OrParallelCoordinator::prunePrefix(const std::string_view deweyPrefix) {
  std::function<void(std::string_view)> broadcastCb;
  std::size_t cancelled = 0;
  {
    std::scoped_lock lock(mu_);
    prunedPrefixes_.emplace_back(deweyPrefix);
    const auto before = frontierQueue_.size();
    std::erase_if(frontierQueue_, [&](const OrParallelSpark &s) {
      return isDeweySubtreeOf(s.deweyKey, deweyPrefix);
    });
    cancelled += (before - frontierQueue_.size());
    rebuildFrontierRankLocked();

    for (auto &[key, stopSource] : activeStopSources_) {
      if (isDeweySubtreeOf(key, deweyPrefix) && !stopSource.stop_requested()) {
        stopSource.request_stop();
        cancelled++;
      }
    }
    broadcastCb = cutBroadcastCallback_;
  }
  if (broadcastCb) {
    broadcastCb(deweyPrefix);
  }
  return cancelled;
}

bool OrParallelCoordinator::isPruned(const std::string_view deweyKey) const {
  std::scoped_lock lock(mu_);
  return std::ranges::any_of(prunedPrefixes_, [&](const std::string &prefix) {
    return isDeweySubtreeOf(deweyKey, prefix);
  });
}

bool OrParallelCoordinator::submitSolution(
    const std::span<const std::uint16_t> deweyPath,
    const std::string_view value) {
  std::scoped_lock lock(mu_);
  const std::string key = formatDeweyPath(deweyPath);
  for (const auto &prefix : prunedPrefixes_) {
    if (isDeweySubtreeOf(key, prefix)) {
      return false;
    }
  }
  std::vector<std::uint16_t> pathVec(deweyPath.begin(), deweyPath.end());
  const CellRef solCell = arena_.makeCell(value);
  solutions_[pathVec]   = OrParallelSolution{
        .deweyPath      = pathVec,
        .deweyKey       = key,
        .value          = std::string(value),
        .submissionCell = solCell,
  };
  rebuildSubmissionRankLocked();
  return true;
}

ExecutionResult OrParallelCoordinator::executeSparkOnWorker(
    const OrParallelSpark &spark, const Manifold *base,
    const std::function<void(VortexCore &, VortexVM &)> &bindProgramOpcodes,
    const std::function<std::optional<std::string>(VortexCore &, VortexVM &)>
        &extractSolution,
    OrParallelCoordinator &coordinator, std::stop_token stopToken,
    const std::size_t maxCycles) {
  if (stopToken.stop_requested() || coordinator.isPruned(spark.deweyKey)) {
    return ExecutionResult{.success           = false,
                           .contractViolation = ContractViolationKind::None,
                           .failingClause     = std::nullopt,
                           .errorMessage      = "Cancelled by cut"};
  }

  ArenaManifold workerArena{base};
  VortexCore workerCore{workerArena};
  if (spark.model == SparkModelKind::ShadowDeltaPrefix &&
      spark.prefixSnapshot.has_value()) {
    workerArena.hydratePrefix(*spark.prefixSnapshot);
  }
  VortexVM workerVm{workerCore};
  if (bindProgramOpcodes) {
    bindProgramOpcodes(workerCore, workerVm);
  }
  workerVm.setCutCallback([&coordinator](std::string_view prefix) {
    coordinator.prunePrefix(prefix);
  });

  std::optional<CellRef> cursor = spark.cursor;
  if (spark.model == SparkModelKind::OracleRecompute) {
    workerVm.setOraclePath(spark.deweyPath);
    if (!cursor || !workerArena.contains(*cursor)) {
      cursor = workerVm.spawnCursor(spark.entryOpcode, "oracle_worker");
    } else {
      workerVm.setCursorOpcode(*cursor, spark.entryOpcode);
    }
  } else {
    workerVm.setCurrentDeweyPath(spark.deweyPath);
    if (!cursor || !workerArena.contains(*cursor)) {
      cursor = workerVm.spawnCursor(spark.entryOpcode, "shadow_worker");
    } else {
      workerVm.setCursorOpcode(*cursor, spark.entryOpcode);
      if (spark.stackFrame && workerArena.contains(*spark.stackFrame)) {
        zigzag::expectWritten(workerArena.link(*cursor, workerCore.dims().stack,
                                               false, *spark.stackFrame));
      }
    }
  }

  if (!cursor) {
    return ExecutionResult{.success           = false,
                           .contractViolation = ContractViolationKind::None,
                           .failingClause     = std::nullopt,
                           .errorMessage = "Could not create worker cursor"};
  }

  ExecutionResult lastResult{.success = false};
  bool anySuccess = false;
  while (!stopToken.stop_requested() && !coordinator.isPruned(spark.deweyKey)) {
    auto res   = workerVm.run(*cursor, maxCycles, stopToken);
    lastResult = res;
    if (res.success && !stopToken.stop_requested()) {
      anySuccess = true;
      if (extractSolution) {
        if (auto sol = extractSolution(workerCore, workerVm); sol.has_value()) {
          coordinator.submitSolution(workerVm.currentDeweyPath(), *sol);
        }
      }
    }
    if (!workerVm.backtrack(*cursor)) {
      break;
    }
  }
  if (anySuccess) {
    return ExecutionResult{.success           = true,
                           .contractViolation = ContractViolationKind::None,
                           .failingClause     = std::nullopt,
                           .errorMessage      = "Finished"};
  }
  return lastResult;
}

std::vector<OrParallelSolution>
OrParallelCoordinator::orderedSolutions() const {
  std::scoped_lock lock(mu_);
  std::vector<OrParallelSolution> out;
  out.reserve(solutions_.size());
  for (const auto &[path, sol] : solutions_) {
    out.push_back(sol);
  }
  return out;
}

std::vector<CellRef> OrParallelCoordinator::submissionRankCells() const {
  std::scoped_lock lock(mu_);
  return zigzag::rankAfter(arena_, goalRoot_, dimSubmission_) |
         std::ranges::to<std::vector>();
}

std::vector<CellRef> OrParallelCoordinator::frontierRankCells() const {
  std::scoped_lock lock(mu_);
  return zigzag::rankAfter(arena_, goalRoot_, dimFrontier_) |
         std::ranges::to<std::vector>();
}

void OrParallelCoordinator::rebuildFrontierRankLocked() {
  zigzag::expectWritten(
      arena_.link(goalRoot_, dimFrontier_, false, std::nullopt));
  CellRef tail = goalRoot_;
  for (const auto &spark : frontierQueue_) {
    const CellRef cell = arena_.makeCell(spark.deweyKey);
    zigzag::expectWritten(arena_.link(tail, dimFrontier_, false, cell));
    tail = cell;
  }
}

void OrParallelCoordinator::rebuildSubmissionRankLocked() {
  zigzag::expectWritten(
      arena_.link(goalRoot_, dimSubmission_, false, std::nullopt));
  CellRef tail = goalRoot_;
  for (const auto &[path, sol] : solutions_) {
    zigzag::expectWritten(
        arena_.link(tail, dimSubmission_, false, std::nullopt));
    zigzag::expectWritten(
        arena_.link(sol.submissionCell, dimSubmission_, true, std::nullopt));
    zigzag::expectWritten(
        arena_.link(sol.submissionCell, dimSubmission_, false, std::nullopt));
    zigzag::expectWritten(
        arena_.link(tail, dimSubmission_, false, sol.submissionCell));
    tail = sol.submissionCell;
  }
}

} // namespace zigzag::vortex

/**
 * @file vql_engine.cpp
 * @brief Direct query evaluation engine for VQL v16.0.
 */
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/functor_capture.hpp"
#include "common/xanadu/vql/invocation.hpp"
#include "common/xanadu/zigzag/query_clone_master.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <concepts>
#include <format>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/view_cells.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include <gleditor/cpp26_concat.hpp>
#include <gleditor/logging.hpp>

namespace xanadu::vql {
using zigzag::DimVector;

// The shape marker dimension is named here rather than by the first shape, so
// the first shape query does not count as minting a dimension and keep its
// whole program (a one-time cost of every cell that program made).
VQLEngine::VQLEngine(MultiStoreCoordinator &coordinator)
    : coordinator_(coordinator), core_(&coordinator.core()) {
  (void)core_->findOrMintDimension(shapeMarkerDimension);
}

VQLEngine::VQLEngine(zigzag::vortex::VortexCore &core)
    : ownedCoordinator_(std::make_unique<MultiStoreCoordinator>(core)),
      coordinator_(*ownedCoordinator_), core_(&core) {
  (void)core_->findOrMintDimension(shapeMarkerDimension);
}

VQLEngine::VQLEngine(zigzag::ArenaManifold &arena)
    : ownedCore_(std::make_unique<zigzag::vortex::VortexCore>(arena)),
      ownedCoordinator_(std::make_unique<MultiStoreCoordinator>(*ownedCore_)),
      coordinator_(*ownedCoordinator_), core_(ownedCore_.get()) {
  (void)core_->findOrMintDimension(shapeMarkerDimension);
}

zigzag::DimRef VQLEngine::resolveDimension(std::string_view name) {
  return coordinator_.resolveDimension(name);
}

void VQLEngine::setVariable(std::string_view name, zigzag::CellRef value) {
  bindName(std::string(name), {value});
}

void VQLEngine::setVariable(std::string_view name,
                            std::vector<zigzag::CellRef> values) {
  bindName(std::string(name), std::move(values));
}

std::vector<zigzag::CellRef>
VQLEngine::getVariable(std::string_view name) const {
  auto it = env_.find(std::string(name));
  if (it != env_.end()) {
    return it->second;
  }
  return {};
}

void VQLEngine::clearVariables() noexcept {
  env_.clear();
  functors_.clear();
  compiledShapes_.clear();
  // Records describe the entries just cleared; a leftover one would condemn
  // a fresh handle that happens to reuse the number.
  shapeEpochs_.clear();
  heldEpochs_.clear();
}

std::unordered_map<std::string, std::vector<PathStep>>
VQLEngine::compilerFunctors() const {
  std::unordered_map<std::string, std::vector<PathStep>> result;
  result.reserve(functors_.size());
  for (const auto &[name, steps] : functors_)
    if (steps) result.emplace(name, *steps);
  return result;
}

void VQLEngine::publishMount(const std::string &dimName, SessionMount mount) {
  const std::string canonical =
      dimName.starts_with("d.") ? dimName : "d." + dimName;
  sessionMounts_[canonical.substr(2)] = mount;
  sessionMounts_[canonical]           = std::move(mount);
}

void VQLEngine::publishUnmount(const std::string &dimName) {
  const std::string canonical =
      dimName.starts_with("d.") ? dimName : "d." + dimName;
  sessionMounts_.erase(canonical);
  sessionMounts_.erase(canonical.substr(2));
}

void VQLEngine::publishCompilerBindings(
    ContractBindings bindings,
    std::unordered_map<std::string, std::vector<PathStep>> functors) {
  auto nextEnv      = env_;
  auto nextFunctors = functors_;
  for (auto &[name, cells] : bindings) {
    nextFunctors.erase(name);
    nextEnv[name] = std::move(cells);
  }
  for (auto &[name, steps] : functors) {
    nextEnv.erase(name);
    nextFunctors[name] =
        std::make_shared<const std::vector<PathStep>>(std::move(steps));
  }
  env_.swap(nextEnv);
  functors_.swap(nextFunctors);
  witnessHeldHandles();
}

void VQLEngine::witnessHeldHandles() {
  // A variable is a bare number, so it carries the epoch its handle was minted
  // in: the table's own witness covers entries, not what a variable names.
  // A witness already recorded for the same variable and cell is kept, so a
  // stale one is not renewed by being published again.
  decltype(heldEpochs_) next;
  for (const auto &[name, cells] : env_)
    for (const auto cell : cells) {
      if (!carriesShapeMarker(arena(), cell)) continue;
      const auto key   = std::pair{name, cell};
      const auto found = heldEpochs_.find(key);
      if (found != heldEpochs_.end()) {
        next.emplace(*found);
      } else if (const auto born = arena().epochOfCell(cell)) {
        next.emplace(key, *born);
      }
    }
  heldEpochs_.swap(next);
}

std::vector<zigzag::CellRef> VQLEngine::pruneStaleShapes() {
  std::vector<zigzag::CellRef> dropped;
  // A variable whose handle's cell was released (and may now be a different
  // cell) is rebound to a marked handle nothing records, so reading it refuses
  // by name instead of meaning whatever now sits at that number.
  for (auto &[name, cells] : env_)
    for (auto &cell : cells) {
      const auto held = heldEpochs_.find(std::pair{name, cell});
      if (held == heldEpochs_.end() ||
          arena().epochOfCell(cell) == held->second)
        continue;
      GLEDITOR_LOG_DEBUG("vql.execution", "variable {} held handle {}: {}",
                         name, cell, staleHandleReason);
      heldEpochs_.erase(held);
      const auto poisoned = arena().makeCell();
      zigzag::expectWritten(arena().link(
          poisoned, core_->findOrMintDimension(shapeMarkerDimension),
          zigzag::DimVector::POS, poisoned));
      cell = poisoned;
      heldEpochs_.emplace(std::pair{name, cell}, *arena().epochOfCell(cell));
    }
  const auto stale = [&](const zigzag::CellRef cell) {
    const auto recorded = shapeEpochs_.find(cell);
    return recorded != shapeEpochs_.end() &&
           arena().epochOfCell(cell) != recorded->second;
  };
  for (auto it = compiledShapes_.begin(); it != compiledShapes_.end();) {
    if (stale(it->first)) {
      GLEDITOR_LOG_DEBUG("vql.execution", "dropped handle {}: {}", it->first,
                         staleHandleReason);
      dropped.push_back(it->first);
      shapeEpochs_.erase(it->first);
      it = compiledShapes_.erase(it);
    } else {
      ++it;
    }
  }
  return dropped;
}

void VQLEngine::publishShapes(ShapeTable shapes,
                              const std::span<const zigzag::CellRef> results) {
  std::erase_if(shapes, [&](const auto &entry) {
    const auto born = arena().epochOfCell(entry.first);
    if (!born) {
      // Nothing to record an epoch from, and a later cell minted at this number
      // would otherwise inherit the entry unchecked.
      GLEDITOR_LOG_DEBUG("vql.execution", "dropped handle {}: {}", entry.first,
                         notACellReason);
      return true;
    }
    const auto recorded = shapeEpochs_.find(entry.first);
    if (recorded == shapeEpochs_.end() || *born == recorded->second)
      return false;
    GLEDITOR_LOG_DEBUG("vql.execution", "dropped handle {}: {}", entry.first,
                       staleHandleReason);
    return true;
  });
  std::unordered_set<zigzag::CellRef> named(results.begin(), results.end());
  for (const auto &[name, cells] : env_)
    named.insert(cells.begin(), cells.end());
  // The computed rules of a view are keyed by the view cell, which a handle a
  // caller keeps names after a later query has run, so they live as long as
  // that cell does; shapes and lenses live as long as a variable or a result
  // names them.
  std::erase_if(shapes, [&](const auto &entry) {
    if (std::holds_alternative<ComputedViewRules>(entry.second))
      return !arena().contains(entry.first);
    return !named.contains(entry.first);
  });
  std::unordered_map<zigzag::CellRef, zigzag::ArenaManifold::ReleaseEpoch>
      epochs;
  for (const auto &entry : shapes) {
    const auto recorded = shapeEpochs_.find(entry.first);
    if (recorded != shapeEpochs_.end()) {
      epochs.emplace(*recorded);
    } else if (const auto born = arena().epochOfCell(entry.first)) {
      epochs.emplace(entry.first, *born);
    }
  }
  compiledShapes_.swap(shapes);
  shapeEpochs_.swap(epochs);
}

void VQLEngine::publishUserDefs(const std::vector<DefDeclaration> &defs) {
  for (const auto &def : defs) {
    userDefs_[def.name] = def;
  }
}

namespace {

/// The one place a QueryError becomes an exception.
[[noreturn]] void throwQueryError(const QueryError &error) {
  switch (error.category) {
  case QueryErrorCategory::TypeContract:
    throw TypeContractError(error.message);
  case QueryErrorCategory::Parse:
    throw ParseError(error.message, SourceLocation{});
  case QueryErrorCategory::Malformed:
    throw std::invalid_argument(error.message);
  case QueryErrorCategory::Unsupported:
  case QueryErrorCategory::Unknown:
  case QueryErrorCategory::Runtime:
  case QueryErrorCategory::Budget:
    break;
  }
  throw QueryRefusal(error.category, error.message);
}

std::vector<zigzag::CellRef>
valueOrThrow(std::expected<std::vector<zigzag::CellRef>, QueryError> result) {
  if (!result) throwQueryError(result.error());
  return std::move(*result);
}

} // namespace

std::expected<std::vector<zigzag::CellRef>, QueryError>
VQLEngine::tryExecute(std::string_view queryString) {
  try {
    Parser parser(queryString);
    return tryExecute(parser.parseQuery());
  } catch (const ParseError &error) {
    return std::unexpected(QueryError{.category = QueryErrorCategory::Parse,
                                      .message  = error.what()});
  }
}

std::expected<std::vector<zigzag::CellRef>, QueryError>
VQLEngine::tryExecute(const QueryExpression &query) {
  zigzag::vortex::VortexVM vm(*core_);
  QueryExecutor executor(*this, vm);
  return executor.execute(query);
}

std::vector<zigzag::CellRef> VQLEngine::execute(std::string_view queryString) {
  // Parsed here, not in tryExecute, so a ParseError keeps its location.
  Parser parser(queryString);
  QueryExpression query = parser.parseQuery();
  return execute(query);
}

std::vector<zigzag::CellRef> VQLEngine::execute(const QueryExpression &query) {
  return valueOrThrow(tryExecute(query));
}

void VQLEngine::bindName(const std::string &name,
                         std::vector<zigzag::CellRef> cells) {
  functors_.erase(name);
  for (auto it = heldEpochs_.begin(); it != heldEpochs_.end();)
    it = it->first.first == name ? heldEpochs_.erase(it) : std::next(it);
  env_[name] = std::move(cells);
  witnessHeldHandles();
}

std::vector<zigzag::CellRef>
VQLEngine::executeStatement(const Statement &stmt) {
  QueryExpression query;
  query.body.statements.push_back(stmt);
  return execute(query);
}

std::vector<VQLEngine::WatchEvent> VQLEngine::pollWatch(const WatchId id) {
  return pollWatch(id, [](const WatchEvent &) {});
}

std::vector<VQLEngine::WatchEvent> VQLEngine::pollWatch(
    const WatchId id,
    const gleditor::cpp26::function_ref<void(const WatchEvent &)> onEvent) {
  const auto found = watches_.find(id);
  if (found == watches_.end())
    throw std::out_of_range("unknown VQL watch subscription");
  if (found->second.polling)
    throw std::logic_error("VQL watch subscription is already being polled");

  const PathExpression path = found->second.path;
  const auto current        = evaluatePath(path);
  std::unordered_map<zigzag::CellRef, WatchSubscription::Snapshot> next;
  std::vector<zigzag::CellRef> nextOrder;
  next.reserve(current.size());
  nextOrder.reserve(current.size());
  for (const auto cell : current) {
    WatchSubscription::Snapshot snapshot{.value    = core_->render(cell),
                                         .topology = watchTopology(cell)};
    if (next.emplace(cell, std::move(snapshot)).second)
      nextOrder.push_back(cell);
  }
  std::vector<WatchEvent> events;
  for (const auto cell : nextOrder) {
    const auto &snapshot = next.at(cell);
    const auto previous  = found->second.previous.find(cell);
    if (previous == found->second.previous.end()) {
      events.push_back({.cell       = cell,
                        .change     = WatchChange::Added,
                        .afterValue = snapshot.value});
      continue;
    }
    const bool valueChanged    = previous->second.value != snapshot.value;
    const bool topologyChanged = previous->second.topology != snapshot.topology;
    if (valueChanged || topologyChanged) {
      events.push_back({.cell            = cell,
                        .change          = WatchChange::Modified,
                        .beforeValue     = previous->second.value,
                        .afterValue      = snapshot.value,
                        .topologyChanged = topologyChanged});
    }
  }
  for (const auto cell : found->second.order) {
    const auto previous = found->second.previous.find(cell);
    if (previous != found->second.previous.end() && !next.contains(cell)) {
      events.push_back({.cell        = cell,
                        .change      = WatchChange::Removed,
                        .beforeValue = previous->second.value});
    }
  }
  found->second.polling = true;
  try {
    for (const auto &event : events) onEvent(event);
  } catch (...) {
    if (const auto active = watches_.find(id); active != watches_.end())
      active->second.polling = false;
    throw;
  }

  // Advance after callbacks so writes caused by this batch are part of the
  // next baseline and cannot recursively trigger the same subscription.
  auto active = watches_.find(id);
  if (active == watches_.end()) return events;
  try {
    const auto settled = evaluatePath(path);
    next.clear();
    nextOrder.clear();
    for (const auto cell : settled) {
      WatchSubscription::Snapshot snapshot{.value    = core_->render(cell),
                                           .topology = watchTopology(cell)};
      if (next.emplace(cell, std::move(snapshot)).second)
        nextOrder.push_back(cell);
    }
  } catch (...) {
    if (const auto stillActive = watches_.find(id);
        stillActive != watches_.end()) {
      stillActive->second.polling = false;
    }
    throw;
  }
  active->second.previous = std::move(next);
  active->second.order    = std::move(nextOrder);
  active->second.polling  = false;
  return events;
}

bool VQLEngine::cancelWatch(const WatchId id) noexcept {
  return watches_.erase(id) != 0;
}

VQLEngine::WatchSourceKey
VQLEngine::watchSourceKey(const zigzag::CellRef cell) const {
  if (const auto source = arena().resolveSourceCell(cell))
    return {source->first, source->second};
  return {nullptr, cell};
}

zigzag::DimRef VQLEngine::arenaDimensionFor(const zigzag::CellRef cell,
                                            const zigzag::DimRef foreignDim) {
  const auto foreign = arena().foreignOf(cell);
  if (!foreign) return foreignDim;
  if (const auto bound = arena().arenaDimFor(foreign->space, foreignDim))
    return *bound;
  const auto space = arena().spaceAt(foreign->space);
  if (!space || !space->manifold || !space->reader) return foreignDim;
  const auto name = space->manifold->textOf(foreignDim, *space->reader);
  if (name.empty()) return foreignDim;
  const auto arenaDim = resolveDimension(name);
  arena().bindDimension(arenaDim, foreign->space, foreignDim);
  return arenaDim;
}

std::vector<VQLEngine::WatchTopologyLink>
VQLEngine::watchTopology(const zigzag::CellRef cell) {
  if (const auto foreign = arena().foreignOf(cell)) {
    const auto space = arena().spaceAt(foreign->space);
    if (space && space->manifold) {
      std::vector<WatchTopologyLink> topology;
      std::unordered_set<zigzag::DimRef> localDimensions;
      for (const auto foreignDim : space->manifold->dimensions()) {
        const auto arenaDim = arenaDimensionFor(cell, foreignDim);
        localDimensions.insert(arenaDim);
        const auto pos =
            arena().linkedMinting(cell, arenaDim, zigzag::DimVector::POS);
        const auto neg =
            arena().linkedMinting(cell, arenaDim, zigzag::DimVector::NEG);
        if (!pos && !neg) continue;
        topology.push_back(
            {.dim = {space->store, foreignDim},
             .pos = pos ? watchSourceKey(*pos)
                        : WatchSourceKey{nullptr, zigzag::noCell},
             .neg = neg ? watchSourceKey(*neg)
                        : WatchSourceKey{nullptr, zigzag::noCell}});
      }
      const auto localLinks = arena().dimensionsOf(cell);
      for (const auto &link : localLinks) {
        if (arena().dimIn(foreign->space, link.dim)) continue;
        if (!localDimensions.insert(link.dim).second) continue;
        const auto pos =
            arena().linkedMinting(cell, link.dim, zigzag::DimVector::POS);
        const auto neg =
            arena().linkedMinting(cell, link.dim, zigzag::DimVector::NEG);
        if (!pos && !neg) continue;
        topology.push_back(
            {.dim = watchSourceKey(link.dim),
             .pos = pos ? watchSourceKey(*pos)
                        : WatchSourceKey{nullptr, zigzag::noCell},
             .neg = neg ? watchSourceKey(*neg)
                        : WatchSourceKey{nullptr, zigzag::noCell}});
      }
      return topology;
    }
  }
  const auto links = arena().dimensionsOf(cell);
  std::vector<WatchTopologyLink> topology;
  topology.reserve(links.size());
  for (const auto &link : links) {
    topology.push_back({.dim = watchSourceKey(link.dim),
                        .pos = watchSourceKey(link.pos),
                        .neg = watchSourceKey(link.neg)});
  }
  return topology;
}

std::vector<zigzag::CellRef>
VQLEngine::evaluatePath(const PathExpression &path,
                        const std::vector<zigzag::CellRef> &contextCells) {
  QueryExpression query;
  query.expr = path;
  zigzag::vortex::VortexVM vm(*core_);
  QueryExecutor executor(*this, vm);
  return valueOrThrow(executor.execute(query, contextCells));
}

std::vector<zigzag::CellRef>
VQLEngine::evaluateStep(const PathStep &step,
                        const std::vector<zigzag::CellRef> &inputCells) {
  PathExpression path;
  path.anchor.kind = AnchorKind::Context;
  path.steps.push_back(step);
  return evaluatePath(path, inputCells);
}

} // namespace xanadu::vql

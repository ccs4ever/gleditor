// Q3: version pins, version values, historical anchors, store generators.
//
// The Vortex VM has no opcode that maps a runtime cell into a historical fold
// (see compileVersionPinnedStep), so history is resolved while compiling, and
// only for cells the compiler already holds by identity: an anchor. Folding a
// store at a microversion is store plumbing, the same kind of work as
// resolving a named store in compileAnchor, not evaluation of the query.
#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <stdexcept>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/zigzag/query_clone_master.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

namespace {

/// Name of the stream binding that carries pinned anchor cells into the
/// ordinary path compiler. A '#' cannot start a user variable, so it cannot
/// collide with one.
constexpr std::string_view pinnedAnchorBinding = "#pinned-anchor";

[[noreturn]] void refuse(const std::string &message) {
  throw QueryRefusal(QueryErrorCategory::Unsupported, message);
}

constexpr std::string_view pathStepPinRefusal =
    "version pins on a path step are not lowered: they must map each cell the "
    "step finds at run time into a historical fold, and the Vortex VM has no "
    "opcode for that; pin the anchor instead (##@(version)/...)";

/// Text of a version variable, or nullopt when the variable is not a value the
/// compiler can read (a runtime register holds no text at compile time).
using VariableText = gleditor::cpp26::function_ref<std::optional<std::string>(
    const std::string &)>;

/// A version variable's text, when it is bound to a cell that already holds
/// one while compiling.
std::optional<std::string> versionVariable(
    const zigzag::vortex::VortexCore &core,
    const std::unordered_map<std::string, std::optional<CellRef>> &bindings,
    const std::string &name) {
  const auto bound = bindings.find(name);
  if (bound == bindings.end() || !bound->second) return std::nullopt;
  const auto value = core.render(*bound->second);
  if (!value || !std::holds_alternative<std::string>(*value))
    return std::nullopt;
  return std::get<std::string>(*value);
}

std::optional<MicroversionId> parseVersion(const std::string &text) {
  try {
    return MicroversionId::parse(text);
  } catch (const std::invalid_argument &) {
    return std::nullopt;
  }
}

/// The first state of branch @p letters off @p version. The branch is spelled
/// into an identifier and read back by MicroversionId::parse, which owns the
/// bijective base-26 rules and the 32-bit limit, so they are not copied here.
std::optional<MicroversionId> branchOf(const MicroversionId &version,
                                       const std::string &letters) {
  // An empty name would parse as the next number on the chain, and state zero
  // has no segment for a branch to hang from.
  if (letters.empty() || version.isZero()) return std::nullopt;
  return parseVersion(version.str() + letters + "1");
}

/// Mirrors VQLEngine::evaluateVersionExpr, reading the version variables the
/// compiler bound instead of the engine's environment.
std::optional<MicroversionId> evaluateVersion(const VersionExpr &expr,
                                              VariableText variable,
                                              zigzag::ArenaManifold &arena) {
  using Op = VersionExpr::Op;
  switch (expr.op) {
  case Op::Range:
    return std::nullopt;
  case Op::Literal:
    return parseVersion(expr.versionStr);
  case Op::Variable: {
    const auto text = variable(expr.versionStr);
    if (!text)
      refuse("version variable '$" + expr.versionStr +
             "' is not a compile-time constant; a version pin needs its "
             "microversion while the query is compiled");
    return parseVersion(*text);
  }
  case Op::Next:
  case Op::Parent: {
    if (!expr.left) return std::nullopt;
    auto version = evaluateVersion(*expr.left, variable, arena);
    if (!version || expr.stepOffset < 0) return std::nullopt;
    for (std::int64_t i = 0; i < expr.stepOffset; ++i) {
      arena.consumeWork();
      *version = expr.op == Op::Next ? version->next() : version->parent();
    }
    return version;
  }
  case Op::Branch: {
    if (!expr.left) return std::nullopt;
    const auto version = evaluateVersion(*expr.left, variable, arena);
    if (!version) return std::nullopt;
    return branchOf(*version, expr.branchName);
  }
  }
  return std::nullopt;
}

const StoreInfo *storeForCell(const MultiStoreCoordinator &coordinator,
                              const zigzag::ArenaManifold &arena,
                              const CellRef cell) {
  const auto stores = coordinator.stores();
  if (const auto source = arena.resolveSourceCell(cell)) {
    const auto found = std::ranges::find_if(stores, [&](const StoreInfo &info) {
      return info.store.get() == source->first;
    });
    if (found != stores.end()) return &*found;
  }
  if (stores.empty()) return nullptr;
  const auto primary =
      std::ranges::find(stores, std::string_view("primary"), &StoreInfo::role);
  return primary == stores.end() ? &stores.front() : &*primary;
}

/// A fold replays up to every operation of the store, so it costs that many
/// units of the arena's work budget; a pin over a long range then fails as a
/// budget refusal instead of running unmetered.
void chargeFold(const zigzag::ArenaManifold &arena, const Store &store) {
  if (!arena.executionLimited()) return;
  try {
    for (std::size_t op = 0; op < store.opCount(); ++op) arena.consumeWork();
  } catch (const std::runtime_error &) {
    throw QueryRefusal(QueryErrorCategory::Budget,
                       "version pin fold exceeds the VQL work budget");
  }
}

/// The space holding the store folded at @p version, folding and attaching it
/// on first use. One fold per (store, version) gives every cell one historical
/// identity however often it is pinned. nullopt when that state has no home.
std::optional<std::uint32_t> historicalSpace(zigzag::ArenaManifold &arena,
                                             const StoreInfo &info,
                                             const MicroversionId &version) {
  const auto label = info.label + "@" + version.str();
  for (std::uint32_t id = 1; id <= arena.spaceCount(); ++id) {
    const auto space = arena.spaceAt(id);
    if (space && space->store == info.store.get() && space->label == label)
      return id;
  }
  chargeFold(arena, *info.store);
  auto fold =
      std::make_shared<zigzag::Manifold>(info.store->rebuildManifold(version));
  if (!fold->home()) return std::nullopt;
  const auto id = arena.attach(zigzag::Space{
      .manifold      = fold.get(),
      .ownedManifold = fold,
      .store         = info.store.get(),
      .reader        = info.store.get(),
      .label         = label,
  });
  arena.bindDimensionsByNameMatch();
  return id;
}

/// The versions a pin names, in pin order. Mirrors the pin handling of
/// VQLEngine::applyVersionPin.
std::vector<MicroversionId> pinnedVersions(const VersionPin &pin,
                                           VariableText variable,
                                           zigzag::ArenaManifold &arena) {
  std::vector<MicroversionId> versions;
  if (pin.isRange && pin.versions.size() >= 2) {
    const auto first = evaluateVersion(pin.versions[0], variable, arena);
    const auto last  = evaluateVersion(pin.versions[1], variable, arena);
    if (!first || !last || !first->isAncestorOf(*last)) return versions;
    for (const auto &version : last->path()) {
      arena.consumeWork();
      if (*first <= version && version <= *last) versions.push_back(version);
    }
    return versions;
  }
  for (const auto &expr : pin.versions) {
    arena.consumeWork();
    if (const auto version = evaluateVersion(expr, variable, arena))
      versions.push_back(*version);
  }
  return versions;
}

/// The historical counterparts of the anchor cells @p inputs, one per input and
/// version whose state holds the cell. Refuses an arena with no store, where a
/// pin would silently name nothing.
std::vector<CellRef> pinAnchorCells(MultiStoreCoordinator &coordinator,
                                    zigzag::ArenaManifold &arena,
                                    const VersionPin &pin,
                                    std::span<const CellRef> inputs,
                                    VariableText variable) {
  if (coordinator.stores().empty())
    refuse("version pins need a store with history; this query runs against "
           "an arena with no store, so there is no microversion to fold");
  std::vector<CellRef> pinned;
  if (pin.versions.empty()) return pinned;
  // Resolved before anything is folded or attached, so a pin the compiler
  // must refuse (a run-time version variable) refuses with the arena intact.
  const auto versions = pinnedVersions(pin, variable, arena);
  for (const CellRef input : inputs) {
    arena.consumeWork();
    const auto *info = storeForCell(coordinator, arena, input);
    if (!info || !info->store) continue;
    const auto available = info->store->allVersions();
    for (const auto &version : versions) {
      arena.consumeWork();
      if (std::ranges::find(available, version) == available.end()) continue;
      const auto space = historicalSpace(arena, *info, version);
      if (!space) continue;
      const auto attached = arena.spaceAt(*space);
      if (!attached || !attached->manifold) continue;
      if (const auto source = arena.resolveSourceCell(input)) {
        if (source->first == info->store.get() &&
            attached->manifold->contains(source->second))
          pinned.push_back(arena.proxyFor(*space, source->second));
      } else if (info->homeCell == input) {
        if (const auto home = attached->manifold->home())
          pinned.push_back(arena.proxyFor(*space, *home));
      }
    }
  }
  return pinned;
}

/// Binds a stream name for the span of one compile and puts back what was
/// there before, so a pin nested in a pinned path, or a second pin compiled
/// while the first is open, does not lose the outer binding.
struct ScopedStreamBinding {
  using Map = std::unordered_map<std::string, std::vector<CellRef>>;
  Map &bindings;
  std::string name;
  std::optional<std::vector<CellRef>> previous;
  ScopedStreamBinding(Map &map, std::string key, std::vector<CellRef> cells)
      : bindings(map), name(std::move(key)) {
    if (const auto found = bindings.find(name); found != bindings.end())
      previous = std::move(found->second);
    bindings[name] = std::move(cells);
  }
  ScopedStreamBinding(const ScopedStreamBinding &)            = delete;
  ScopedStreamBinding &operator=(const ScopedStreamBinding &) = delete;
  ~ScopedStreamBinding() {
    if (previous)
      bindings[name] = std::move(*previous);
    else
      bindings.erase(name);
  }
};

} // namespace

std::vector<CellRef>
VQLCompiler::compileVersionPinnedStep(const PathStep & /*step*/,
                                      std::vector<CellRef> /*stream*/,
                                      const bool /*hasContext*/) {
  // A step's output is a set of run-time registers. Mapping one to its
  // counterpart in a fold at another microversion needs the store, the fold
  // and the cell's source index at run time, and no opcode in OpcodeKind
  // reads them: Link, Find and Linked walk topology, they do not cross folds.
  refuse(std::string(pathStepPinRefusal));
}

std::optional<CellRef>
VQLCompiler::compileVersionPinnedPath(const PathExpression &path,
                                      const CellRef ctxCell) {
  if (std::ranges::any_of(path.steps, [](const PathStep &step) {
        return step.versionPin.has_value();
      }))
    refuse(std::string(pathStepPinRefusal));
  if (!coordinator_)
    refuse("version pins need a store with history; this compiler has no "
           "store coordinator, so there is no microversion to fold");

  const auto variable = [this](const std::string &name) {
    return versionVariable(core_, varBindings_, name);
  };

  // The anchor's cells are known while compiling. Pin them here, then hand the
  // pinned cells to the ordinary path compiler as the anchor's stream so
  // predicates, steps, projections and clone tails are compiled once, by the
  // code that already handles them.
  std::vector<CellRef> base;
  switch (path.anchor.kind) {
  case AnchorKind::Home:
  case AnchorKind::Root:
  case AnchorKind::NamedStore:
  case AnchorKind::LiteralCellId:
  case AnchorKind::StoreGenerator:
  case AnchorKind::StoreAnchor:
    if (const auto cell = compileAnchor(path.anchor)) base.push_back(*cell);
    break;
  case AnchorKind::Variable: {
    if (const auto bound = streamBindings_.find(path.anchor.name);
        bound != streamBindings_.end()) {
      base = bound->second;
    } else if (const auto var = varBindings_.find(path.anchor.name);
               var != varBindings_.end() && var->second) {
      base.push_back(*var->second);
    }
    // Only cells the arena already names as store cells can be pinned while
    // compiling; a register is filled at run time.
    if (!std::ranges::all_of(base, [&](const CellRef cell) {
          return core_.arena().foreignOf(cell).has_value() &&
                 !dynamicStreams_.contains(cell);
        }))
      refuse("version pins on a variable bound to run-time cells are not "
             "lowered: the Vortex VM cannot map a run-time cell into a "
             "historical fold");
    break;
  }
  default:
    refuse("version pins on this anchor kind are not lowered; pin a store "
           "anchor (##@(version)/...)");
  }
  if (path.anchor.derefMaster) {
    for (auto &cell : base)
      cell = zigzag::queryCloneMaster(core_.arena(), cell, core_.dims().clone)
                 .value_or(cell);
  }

  std::vector<CellRef> pinned = std::move(base);
  if (path.anchor.versionPin)
    pinned = pinAnchorCells(*coordinator_, core_.arena(),
                            *path.anchor.versionPin, pinned, variable);

  PathExpression rewritten = path;
  rewritten.anchor         = AnchorNode{.kind = AnchorKind::Variable,
                                        .name = std::string(pinnedAnchorBinding)};
  const ScopedStreamBinding binding{
      streamBindings_, std::string(pinnedAnchorBinding), std::move(pinned)};
  return compilePathExpression(rewritten, ctxCell);
}

std::optional<CellRef>
VQLCompiler::compileHistoricalAnchor(const AnchorNode &anchor) {
  const auto currentHome = [this]() -> std::optional<CellRef> {
    if (coordinator_) return coordinator_->homeAnchor();
    return core_.home();
  };
  switch (anchor.kind) {
  case AnchorKind::StoreGenerator: {
    if (!anchor.generator)
      refuse("store generator anchor has no generator call");
    if (anchor.generator->opName == "current") return currentHome();
    // Refused for a store that does not exist as well: an unselected store
    // must never answer with the ambient home. How alternative stores are
    // selected or combined is a language design still to come.
    refuse("store generator '" + anchor.generator->opName +
           "' is not lowered, whether or not the store exists");
  }
  case AnchorKind::StoreAnchor: {
    if (!anchor.storeAnchor) refuse("store anchor has no method");
    if (anchor.storeAnchor->method == "current") return currentHome();
    refuse("store anchor '##" + anchor.storeAnchor->method +
           "(...)' is not lowered, whether or not the store exists");
  }
  case AnchorKind::Microversion: {
    if (!coordinator_ || coordinator_->stores().empty())
      refuse("microversion anchor '@" + anchor.microversionId +
             "' needs a store with history; this query runs against an arena "
             "with no store");
    const auto home = coordinator_->homeAnchor();
    if (!home) return std::nullopt;
    VersionPin pin;
    if (anchor.versionPin) {
      pin = *anchor.versionPin;
    } else {
      VersionExpr version;
      version.versionStr = anchor.microversionId;
      pin.versions.push_back(std::move(version));
    }
    const auto cells =
        pinAnchorCells(*coordinator_, core_.arena(), pin, std::array{*home},
                       [](const std::string &) -> std::optional<std::string> {
                         return std::nullopt;
                       });
    if (cells.size() > 1)
      refuse("a microversion anchor naming several versions yields several "
             "cells; write ##@(v1, v2) so the pinned stream keeps them all");
    if (cells.empty()) return std::nullopt;
    return cells.front();
  }
  default:
    refuse("historical anchor kind is not lowered");
  }
}

std::optional<CellRef>
VQLCompiler::compileVersionValue(const VersionExpr &version,
                                 const CellRef /*ctxCell*/) {
  const auto variable = [this](const std::string &name) {
    return versionVariable(core_, varBindings_, name);
  };
  const auto resolved = evaluateVersion(version, variable, core_.arena());
  if (!resolved) return std::nullopt;
  return emitConstant(zigzag::vortex::CellValue{resolved->str()});
}

bool compareVersionsCausally(const CompOp op, const MicroversionId &lhs,
                             const MicroversionId &rhs) {
  // isAncestorOf is strict, so <= and >= add equality; the engine's direct
  // evaluator forgot to, which made @a <= @a false.
  // Versions form a tree, not a line: siblings are neither before nor after
  // each other, so text or integer order would answer where history cannot.
  const bool lhsFirst = lhs.isAncestorOf(rhs);
  const bool rhsFirst = rhs.isAncestorOf(lhs);
  switch (op) {
  case CompOp::Equal:
    return lhs == rhs;
  case CompOp::NotEqual:
    return lhs != rhs;
  case CompOp::LessThan:
    return lhsFirst && lhs != rhs;
  case CompOp::LessEqual:
    return lhsFirst || lhs == rhs;
  case CompOp::GreaterThan:
    return rhsFirst && lhs != rhs;
  case CompOp::GreaterEqual:
    return rhsFirst || lhs == rhs;
  }
  return false;
}

} // namespace xanadu::vql

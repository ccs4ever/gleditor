/**
 * @file vql_engine.hpp
 * @brief Direct query evaluation engine for VQL v16.0 operating over
 * ArenaManifold and Store.
 */
#ifndef COMMON_XANADU_VQL_VQL_ENGINE_HPP
#define COMMON_XANADU_VQL_VQL_ENGINE_HPP

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/vql/query_error.hpp"
#include "common/xanadu/vql/shape_limits.hpp"
#include <gleditor/cpp26.hpp>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {

using xanadu::MultiStoreCoordinator;

class VQLEngine {
public:
  using WatchId = std::uint64_t;
  enum class WatchChange : std::uint8_t { Added, Removed, Modified };
  struct WatchEvent {
    zigzag::CellRef cell;
    WatchChange change{WatchChange::Added};
    std::optional<zigzag::vortex::CellValue> beforeValue;
    std::optional<zigzag::vortex::CellValue> afterValue;
    bool topologyChanged{false};
  };
  explicit VQLEngine(MultiStoreCoordinator &coordinator);
  explicit VQLEngine(zigzag::vortex::VortexCore &core);
  explicit VQLEngine(zigzag::ArenaManifold &arena);

  /// Primary API: runs a parsed query and returns its cells, or a typed
  /// error. Query failures, parse errors included, are never thrown.
  [[nodiscard]] std::expected<std::vector<zigzag::CellRef>, QueryError>
  tryExecute(const QueryExpression &query);

  /// Primary API for source text (lexing, parsing, and evaluating).
  [[nodiscard]] std::expected<std::vector<zigzag::CellRef>, QueryError>
  tryExecute(std::string_view queryString);

  /// Throwing wrapper over tryExecute: TypeContractError, ParseError,
  /// std::invalid_argument (Malformed) or QueryRefusal (everything else).
  std::vector<zigzag::CellRef> execute(const QueryExpression &query);

  /// Throwing wrapper over tryExecute for source text.
  std::vector<zigzag::CellRef> execute(std::string_view queryString);

  /// Evaluates a path expression starting from an optional set of context
  /// cells.
  std::vector<zigzag::CellRef>
  evaluatePath(const PathExpression &path,
               const std::vector<zigzag::CellRef> &contextCells = {});

  /// Evaluates a single path step over an input set of cells.
  std::vector<zigzag::CellRef>
  evaluateStep(const PathStep &step,
               const std::vector<zigzag::CellRef> &currentCells);

  /// Variable environment management.
  void setVariable(std::string_view name, zigzag::CellRef value);
  void setVariable(std::string_view name, std::vector<zigzag::CellRef> values);
  [[nodiscard]] std::vector<zigzag::CellRef>
  getVariable(std::string_view name) const;
  void clearVariables() noexcept;
  [[nodiscard]] const ContractBindings &variables() const noexcept {
    return env_;
  }
  using FunctorSteps = std::shared_ptr<const std::vector<PathStep>>;
  [[nodiscard]] std::unordered_map<std::string, std::vector<PathStep>>
  compilerFunctors() const;
  /// A dimension mounted for this session: a walk along it that finds nothing
  /// continues from `roots` when the step's direction matches.
  struct SessionMount {
    zigzag::DimVector direction{zigzag::DimVector::POS};
    std::vector<zigzag::CellRef> roots;
  };
  /// The mount table, keyed by both the short and the `d.`-qualified name.
  [[nodiscard]] const std::unordered_map<std::string, SessionMount> &
  compilerMounts() const noexcept {
    return sessionMounts_;
  }
  /// Records a mount under both spellings of @p dimName. Private to this
  /// session: it adds no cell to the arena.
  void publishMount(const std::string &dimName, SessionMount mount);
  /// Removes both spellings of @p dimName.
  void publishUnmount(const std::string &dimName);
  void publishCompilerBindings(
      ContractBindings bindings,
      std::unordered_map<std::string, std::vector<PathStep>> functors);

  /// The handles the compiler may resolve, handle cell to an expanded shape
  /// or a lens pair. See publishShapes for the lifetime.
  using ShapeTable = CompiledHandleTable;
  [[nodiscard]] const ShapeTable &compilerShapes() const noexcept {
    return compiledShapes_;
  }
  /// Number of shape handles the session can still resolve.
  [[nodiscard]] std::size_t shapeCount() const noexcept {
    return compiledShapes_.size();
  }
  /// Adopts @p shapes after a query succeeded and its variables were
  /// published, keeping only the handles a variable or one of @p results
  /// still names. That is what bounds the table by the shapes held rather
  /// than by the queries run. Called with nothing on failure, so a failed
  /// query registers no shape.
  void publishShapes(ShapeTable shapes,
                     std::span<const zigzag::CellRef> results);
  /// Why a table entry is dropped when its key no longer names the cell it was
  /// published for.
  static constexpr std::string_view staleHandleReason =
      "the handle's cell was released and its number now names a different "
      "cell";
  /// Why an entry is refused when its key is not a cell of the arena.
  static constexpr std::string_view notACellReason =
      "the handle's key is not a cell of the arena";
  /// Drops every entry whose cell was minted in a different release epoch than
  /// the one recorded when it was published, so a released handle's number,
  /// reused by a new cell, cannot hand that cell the old entry. A live handle
  /// keeps its entry across any number of releases of cells above it. Returns
  /// the keys dropped. Run at the start of every query, and by publishShapes
  /// on what it is handed.
  std::vector<zigzag::CellRef> pruneStaleShapes();
  [[nodiscard]] const std::unordered_map<std::string, DefDeclaration> &
  userDefs() const noexcept {
    return userDefs_;
  }
  void publishUserDefs(const std::vector<DefDeclaration> &defs);

  /// Executes a single statement in the query.
  std::vector<zigzag::CellRef> executeStatement(const Statement &stmt);

  /// Executes a perspective block through the compile-and-run path.
  std::vector<zigzag::CellRef>
  executePerspectiveBlock(const PerspectiveBlock &block);

  /// Executes a one-shot local watch snapshot, optionally diffed from `since`.
  std::vector<zigzag::CellRef> executeWatch(const WatchStatement &watchStmt);

  /// Register a local snapshot query for explicit polling. The first poll
  /// reports its current matches; later polls report membership, value and
  /// topology changes. The callback overload advances the baseline after
  /// callbacks so their writes do not trigger the same subscription again.
  WatchId subscribeWatch(PathExpression path);
  [[nodiscard]] std::vector<WatchEvent> pollWatch(WatchId id);
  [[nodiscard]] std::vector<WatchEvent>
  pollWatch(WatchId id,
            gleditor::cpp26::function_ref<void(const WatchEvent &)> onEvent);
  bool cancelWatch(WatchId id) noexcept;

  /// Records a session mount declaration.
  void executeMount(const MountDeclaration &mount);
  /// Removes a session mount alias without changing any store.
  void executeUnmount(const UnmountDeclaration &unmount);

  /// The ceilings a shape is expanded, validated and written to cells under.
  /// A host that has read system://settings hands them over here; without it
  /// the defaults in shape_limits.hpp apply.
  VQLEngine *setShapeLimits(const ShapeLimits &limits) noexcept {
    shapeLimits_ = limits;
    return this;
  }
  [[nodiscard]] const ShapeLimits &shapeLimits() const noexcept {
    return shapeLimits_;
  }

  /// Resolves dimension name to DimRef, minting dynamically if new.
  zigzag::DimRef resolveDimension(std::string_view name);

  [[nodiscard]] MultiStoreCoordinator &coordinator() noexcept {
    return coordinator_;
  }
  [[nodiscard]] const MultiStoreCoordinator &coordinator() const noexcept {
    return coordinator_;
  }
  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return *core_; }
  [[nodiscard]] const zigzag::vortex::VortexCore &core() const noexcept {
    return *core_;
  }
  [[nodiscard]] zigzag::ArenaManifold &arena() noexcept {
    return core_->arena();
  }
  [[nodiscard]] const zigzag::ArenaManifold &arena() const noexcept {
    return core_->arena();
  }

private:
  // Compile-time shape and lens table (decision D4 of the VM restoration):
  // shapes and lens callbacks are syntax, not data, so the handle cell stands
  // for a ShapeLiteral or a LensPair the compiler resolves while lowering and
  // the VM never sees.
  // It is an exception to the "cells and dimensions, not side tables" rule,
  // taken because those paths consume syntax trees; moving the data half into
  // cells is a later card. It follows env_: clearVariables() empties it.
  ShapeTable compiledShapes_;
  ShapeLimits shapeLimits_{};
  /// The epoch each key of compiledShapes_ was minted in when published: the
  /// witness that the number still names that cell.
  std::unordered_map<zigzag::CellRef, zigzag::ArenaManifold::ReleaseEpoch>
      shapeEpochs_;
  /// The epoch of each marked handle a variable holds, keyed by the variable
  /// and the cell, from when it was bound; pruneStaleShapes compares them.
  std::map<std::pair<std::string, zigzag::CellRef>,
           zigzag::ArenaManifold::ReleaseEpoch>
      heldEpochs_;
  void witnessHeldHandles();

  [[nodiscard]] zigzag::DimRef arenaDimensionFor(zigzag::CellRef cell,
                                                 zigzag::DimRef foreignDim);

  /// Binds a parameter name to an argument, as a functor when it is one.
  void bindName(const std::string &name, std::vector<zigzag::CellRef> cells);

  std::unique_ptr<zigzag::vortex::VortexCore> ownedCore_{nullptr};
  std::unique_ptr<MultiStoreCoordinator> ownedCoordinator_{nullptr};
  MultiStoreCoordinator &coordinator_;
  zigzag::vortex::VortexCore *core_{nullptr};

  std::unordered_map<std::string, std::vector<zigzag::CellRef>> env_;
  /// Names bound to functors rather than cells; a name is in one or the other.
  std::unordered_map<std::string, FunctorSteps> functors_;
  // Runtime-only aliases; mount dispatch must never mint persistent structure.
  std::unordered_map<std::string, SessionMount> sessionMounts_;
  using WatchSourceKey = std::pair<const xanadu::Store *, zigzag::CellRef>;
  struct WatchTopologyLink {
    WatchSourceKey dim;
    WatchSourceKey pos;
    WatchSourceKey neg;
    bool operator==(const WatchTopologyLink &) const = default;
  };
  struct WatchSubscription {
    PathExpression path;
    bool polling{false};
    struct Snapshot {
      std::optional<zigzag::vortex::CellValue> value;
      std::vector<WatchTopologyLink> topology;
    };
    std::unordered_map<zigzag::CellRef, Snapshot> previous;
    std::vector<zigzag::CellRef> order;
  };
  [[nodiscard]] WatchSourceKey watchSourceKey(zigzag::CellRef cell) const;
  [[nodiscard]] std::vector<WatchTopologyLink>
  watchTopology(zigzag::CellRef cell);
  WatchId nextWatchId_{1};
  std::unordered_map<WatchId, WatchSubscription> watches_;
  std::unordered_map<std::string, DefDeclaration> userDefs_;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_VQL_ENGINE_HPP

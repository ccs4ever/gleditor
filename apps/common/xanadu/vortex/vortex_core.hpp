/**
 * @file vortex_core.hpp
 * @brief Core runtime engine and zzstructure manifold primitives for Vortex.
 *
 * Implements the single-primitive invariant (link and value), deterministic
 * system genesis, the dual-wing parameter calling convention, parameter
 * preprocessing and postprocessing pipelines along +d.spin, Design by
 * Contract assertions along d.contract, and ephemeral island pinning
 * along d.pinning-cursors.
 */
#ifndef COMMON_XANADU_VORTEX_CORE_HPP
#define COMMON_XANADU_VORTEX_CORE_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {

using CellValue = std::variant<std::string, double, std::int64_t, bool>;

/**
 * @brief System dimension coordinates minted deterministically off home.
 */
struct SystemDimensions {
  DimRef grab{
      noCell}; ///< Parameter wings (-d.grab = outputs, +d.grab = inputs)
  DimRef step{noCell};           ///< Parameter chaining along rank
  DimRef spin{noCell};           ///< Process instruction stream
  DimRef stack{noCell};          ///< Call frame stack
  DimRef contract{noCell};       ///< Design by Contract (-d.contract = require,
                                 ///< +d.contract = ensure)
  DimRef clone{noCell};          ///< Clone rank (shared identity)
  DimRef cursors{noCell};        ///< Process scheduler manifold
  DimRef cache{noCell};          ///< Memoization cache rank on pinned islands
  DimRef vars{noCell};           ///< Scope variable names
  DimRef values{noCell};         ///< Scope bound values
  DimRef pinningCursors{noCell}; ///< Pinned subgraph root set rank
  DimRef name{noCell};           ///< Thread, pin, and store naming rank
  DimRef role{noCell};           ///< Store and cell role classification rank
  DimRef dims{noCell};           ///< Dimension directory rank off home
  DimRef stdlib{noCell}; ///< Standard library module directory rank off home
  DimRef clause{noCell}; ///< Clause database rank off predicate cells (Vlog)
  DimRef scan{noCell};   ///< String-scanning cursor rank off home; a cursor's
                         ///< content is &pos, its +d.grab &subject (vicon
                         ///< std:goal, spec section 6.1)
  DimRef stores{noCell}; ///< Multi-store connection rank off home
};

enum class ContractViolationKind : std::uint8_t {
  None,
  Precondition,
  Postcondition,
  /// P15 (design/vagent-metaprogramming.md §6.5/M10): the write funnel
  /// refused a program-value write aimed at an opcodeMap_-registered
  /// cell's clone master — kind immutability, which no trail could
  /// undo because the map is not arena state.
  KindImmutability,
};

struct ExecutionResult {
  bool success{true};
  ContractViolationKind contractViolation{ContractViolationKind::None};
  std::optional<CellRef> failingClause;
  std::string errorMessage;
};

/**
 * @brief The typed arm of a writeValue/splice refusal (P15).
 *
 * The funnel's other nullopt — "the arena cannot reach the target" —
 * carries no fault; a write *refused* by the installed kind predicate
 * names why through this channel so each call site can surface a typed
 * `success=false` ExecutionResult.
 */
enum class WriteFault : std::uint8_t {
  /// No refusal — either the write landed or the target is simply not
  /// an arena cell (the legacy nullopt arm).
  None,
  /// The VM-installed kind predicate claimed the target's resolved
  /// clone master: the cell is opcodeMap_-registered, so restating its
  /// content would desync the registered kind (spec §6.5/M10). Fresh
  /// cells are never claimed — an unregistered cell's content is its
  /// kind and cannot desync.
  OpcodeKind,
};

/**
 * @brief Resolves [offset, length] ranges with negative index support.
 * Negative offsets and lengths count from the end of the content.
 */
inline std::pair<std::size_t, std::size_t>
resolve_range(std::size_t size, std::int64_t offset,
              std::optional<std::int64_t> length) {
  const auto signed_size = static_cast<std::int64_t>(size);
  std::int64_t from      = (offset < 0) ? signed_size + offset : offset;
  from                   = std::clamp<std::int64_t>(from, 0, signed_size);

  std::int64_t to = !length         ? signed_size
                    : (*length < 0) ? signed_size + *length + 1
                                    : from + *length;
  to              = std::clamp<std::int64_t>(to, from, signed_size);

  return {static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)};
}

/**
 * @class VortexCore
 * @brief High-performance spatial runtime engine operating over an
 * ArenaManifold.
 */
class VortexCore {
public:
  explicit VortexCore(ArenaManifold &arena);

  // -- System Genesis ---------------------------------------------------------
  [[nodiscard]] CellRef home() const noexcept { return home_; }
  [[nodiscard]] const SystemDimensions &dims() const noexcept { return dims_; }
  [[nodiscard]] ArenaManifold &arena() noexcept { return arena_; }
  [[nodiscard]] const ArenaManifold &arena() const noexcept { return arena_; }

  // -- The Single-Primitive Core ----------------------------------------------
  /// Read the neighbour along a dimension, or nullopt when there is none.
  [[nodiscard]] std::optional<CellRef> readLink(CellRef cell, DimRef dim,
                                                DimVector dir) const;
  [[nodiscard]] std::optional<CellRef> readLink(CellRef cell,
                                                DirectedDim target) const {
    return readLink(cell, target.dim, target.dir);
  }
  [[nodiscard]] std::optional<CellRef> readLink(CellRef cell, DimRef dim,
                                                bool negward) const {
    return readLink(cell, dim, fromNegward(negward));
  }
  /// Establish a link to an existing cell.
  std::optional<CellRef> setLink(CellRef cell, DimRef dim, DimVector dir,
                                 CellRef target);
  std::optional<CellRef> setLink(CellRef cell, DirectedDim target,
                                 CellRef linkTarget) {
    return setLink(cell, target.dim, target.dir, linkTarget);
  }
  std::optional<CellRef> setLink(CellRef cell, DimRef dim, bool negward,
                                 CellRef target) {
    return setLink(cell, dim, fromNegward(negward), target);
  }
  /// Mint a fresh cell and link it to this position.
  std::optional<CellRef> allocateLink(CellRef cell, DimRef dim, DimVector dir);
  std::optional<CellRef> allocateLink(CellRef cell, DirectedDim target) {
    return allocateLink(cell, target.dim, target.dir);
  }
  std::optional<CellRef> allocateLink(CellRef cell, DimRef dim, bool negward) {
    return allocateLink(cell, dim, fromNegward(negward));
  }
  /// Sever a link and return the former neighbour, if one existed.
  std::optional<CellRef> isolateLink(CellRef cell, DimRef dim, DimVector dir);
  std::optional<CellRef> isolateLink(CellRef cell, DirectedDim target) {
    return isolateLink(cell, target.dim, target.dir);
  }
  std::optional<CellRef> isolateLink(CellRef cell, DimRef dim, bool negward) {
    return isolateLink(cell, dim, fromNegward(negward));
  }

  /**
   * @brief Reads, slices, or splices cell content in the universal currency of
   * CellRef.
   *
   * An absent length means the whole remaining value; a supplied negative
   * length retains the language's count-from-end meaning.
   * Read slice (offset/len specified)      -> returns ephemeral cell quoting
   * range. Write (replacement specified)         -> splices on clone master,
   * returns cell.
   */
  std::optional<CellRef>
  readValue(CellRef cell, std::int64_t offset = 0,
            std::optional<std::int64_t> length = std::nullopt);
  /// The write funnel P15 guards (spec §6.5/M10): every program-value
  /// write resolves through this arm. When @p fault is non-null it is
  /// always written — WriteFault::None for a landed write or the
  /// legacy "not an arena cell" nullopt, WriteFault::OpcodeKind when
  /// the installed kind predicate refuses — so the VM can map the
  /// refusal to a typed `success=false` ExecutionResult per site.
  std::optional<CellRef> writeValue(CellRef cell, std::int64_t offset,
                                    std::optional<std::int64_t> length,
                                    const CellValue &replacement,
                                    WriteFault *fault = nullptr);
  /// Install the kind predicate the funnel consults on the resolved
  /// clone master — P15's `opcodeMap_.contains(masterOf(cell))`. The
  /// VortexVM over this core installs it at construction; a second VM
  /// sharing the core re-installs its own map's view.
  VortexCore *setWriteGuard(std::function<bool(CellRef master)> guard) {
    writeGuard_ = std::move(guard);
    return this;
  }

  // -- Named Vocabulary Helpers -----------------------------------------------
  std::optional<CellRef> newCell(CellRef cell, DimRef dim,
                                 DimVector dir = DimVector::POS);
  std::optional<CellRef> newCell(CellRef cell, DirectedDim target) {
    return newCell(cell, target.dim, target.dir);
  }
  std::optional<CellRef> newCell(CellRef cell, DimRef dim, bool negward) {
    return newCell(cell, dim, fromNegward(negward));
  }

  std::optional<CellRef> newCell(CellRef cell, DimRef dim, DimVector dir,
                                 const CellValue &val);
  std::optional<CellRef> newCell(CellRef cell, DirectedDim target,
                                 const CellValue &val) {
    return newCell(cell, target.dim, target.dir, val);
  }
  std::optional<CellRef> newCell(CellRef cell, DimRef dim, bool negward,
                                 const CellValue &val) {
    return newCell(cell, dim, fromNegward(negward), val);
  }

  std::optional<CellRef> breakLink(CellRef cell, DimRef dim,
                                   DimVector dir = DimVector::POS);
  std::optional<CellRef> breakLink(CellRef cell, DirectedDim target) {
    return breakLink(cell, target.dim, target.dir);
  }
  std::optional<CellRef> breakLink(CellRef cell, DimRef dim, bool negward) {
    return breakLink(cell, dim, fromNegward(negward));
  }

  std::optional<CellRef> splice(CellRef cell, std::int64_t offset,
                                std::int64_t length, const CellValue &val,
                                WriteFault *fault = nullptr);
  std::optional<CellRef> insert(CellRef cell, std::int64_t offset,
                                const CellValue &val,
                                WriteFault *fault = nullptr);
  std::optional<CellRef> append(CellRef cell, const CellValue &val,
                                WriteFault *fault = nullptr);
  std::optional<CellRef> erase(CellRef cell, std::int64_t offset,
                               std::int64_t length,
                               WriteFault *fault = nullptr);

  /// Mints a new named dimension and links it to the d.dims rank tail.
  CellRef mintDimension(std::string_view name);

  /// The dimension named @p name on the d.dims rank, minting it onto the
  /// rank's tail when there is none. What each front end (VQL, VPL, the
  /// multi-store coordinator) does for a dimension it has no fixed slot for.
  DimRef findOrMintDimension(std::string_view name);

  /// The dimension called @p name on the d.dims rank, minting nothing.
  [[nodiscard]] std::optional<DimRef>
  findDimension(std::string_view name) const;

  // -- Library modules
  // ---------------------------------------------------------

  /// The module cell named @p modulePath on home's +d.stdlib rank.
  [[nodiscard]] std::optional<CellRef>
  findModule(std::string_view modulePath) const;
  /// findModule(), minting the module onto the rank's tail when absent.
  CellRef getOrCreateModule(std::string_view modulePath);
  /// Export @p entryOp as @p symbolName under @p moduleCell: a symbol cell on
  /// the module's +d.vars rank, with the op on the symbol's +d.values.
  void exportSymbol(CellRef moduleCell, std::string_view symbolName,
                    CellRef entryOp);

  [[nodiscard]] std::optional<CellValue>
  get(CellRef cell, std::int64_t offset = 0,
      std::optional<std::int64_t> length = std::nullopt);
  [[nodiscard]] std::optional<CellValue> render(CellRef cell) const;
  [[nodiscard]] std::optional<bool> isTruthy(CellRef cell) const;
  [[nodiscard]] static bool evaluateTruthiness(const CellValue &val);

  [[nodiscard]] std::function<CellRef()> cloneGenerator(CellRef source);

  // -- Dual-Wing Calling Convention -------------------------------------------
  [[nodiscard]] std::vector<CellRef> inputsOf(CellRef opcode) const;
  [[nodiscard]] std::vector<CellRef> outputsOf(CellRef opcode) const;
  void bindInput(CellRef opcode, CellRef operand);
  void bindOutput(CellRef opcode, CellRef target);

  // -- Parameter Pre/Postprocessing Pipelines ---------------------------------
  [[nodiscard]] bool hasPipeline(CellRef paramCell) const;
  [[nodiscard]] std::optional<CellRef> getPipelineHead(CellRef paramCell) const;
  void attachPipeline(CellRef paramCell, CellRef firstOp);

  // -- Design by Contract -----------------------------------------------------
  [[nodiscard]] std::vector<CellRef> preconditionsOf(CellRef opcode) const;
  [[nodiscard]] std::vector<CellRef> postconditionsOf(CellRef opcode) const;
  void attachPrecondition(CellRef opcode, CellRef conditionOp);
  void attachPostcondition(CellRef opcode, CellRef conditionOp);

  // -- Pinning & Memory Lifecycle ---------------------------------------------
  CellRef pin(std::string_view name, std::optional<CellRef> targetNode);
  [[nodiscard]] std::optional<CellRef> findPin(std::string_view name) const;
  bool flushPin(std::string_view name);
  bool retirePin(std::string_view name);
  std::size_t collectGarbage();

  // -- Vortex-Native Memoization Library Helpers ------------------------------
  CellRef getOrCreateMemoPin(std::string_view opName);
  [[nodiscard]] std::optional<std::vector<CellValue>>
  lookupMemo(CellRef memoPin, const std::vector<CellValue> &inputs);
  void recordMemo(CellRef memoPin, const std::vector<CellValue> &inputs,
                  const std::vector<CellValue> &outputs);
  bool flushMemo(std::string_view opName);
  bool retireMemo(std::string_view opName);

private:
  /// cloneMaster(), total: a cell the arena cannot reach is its own master.
  [[nodiscard]] CellRef masterOf(CellRef cell) const noexcept;
  /// The base manifold's dimension named @p name, when there is a base.
  [[nodiscard]] std::optional<DimRef>
  baseDimensionNamed(std::string_view name) const;
  /// The operands on @p opcode's input (POS) or output (NEG) wing.
  [[nodiscard]] std::vector<CellRef> wingOf(CellRef opcode,
                                            DimVector side) const;
  /// The conditions on @p opcode's pre (NEG) or post (POS) contract rank.
  [[nodiscard]] std::vector<CellRef> contractOf(CellRef opcode,
                                                DimVector side) const;
  void attachContract(CellRef opcode, DimVector side, CellRef conditionOp);

  void initGenesis();
  [[nodiscard]] CellRef mintNamedDimension(std::string_view name,
                                           CellRef &lastDimCell);

  ArenaManifold &arena_;
  CellRef home_{noCell};
  SystemDimensions dims_{};
  /// P15's installed predicate (spec §6.5/M10): consulted on the
  /// resolved clone master inside writeValue — empty while no VortexVM
  /// over this core has installed one.
  std::function<bool(CellRef)> writeGuard_;
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_CORE_HPP

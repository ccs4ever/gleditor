/**
 * @file vortex_host.hpp
 * @brief Standardized public interface bridging the Vortex runtime, standard
 * library, and VQL into Zigzag and Xuzz applications.
 */
#ifndef COMMON_XANADU_VORTEX_HOST_HPP
#define COMMON_XANADU_VORTEX_HOST_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/vpl_engine.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"

namespace zigzag::vortex {

using AppActionDelegate =
    std::function<bool(std::string_view action, CellRef focusCell,
                       ViewAxisBinding &axes, CellRef &newFocusOut)>;

struct VortexHostConfig {
  std::size_t schedulerCycleBudget{1000};
  std::size_t gcIntervalFrames{300};
  std::string commandPromptHotkey{":"};
  std::string paletteHotkey{"F4"};
  std::string defaultBundle{"Execution"};

  [[nodiscard]] static VortexHostConfig fromStore(const xanadu::Store &store);
};

/**
 * @class VortexHost
 * @brief Standardized public interface coordinating VortexCore, VortexVM,
 *        VortexStdLib, VQLEngine, and VQLCompiler over an ArenaManifold
 * overlay.
 */
class VortexHost {
public:
  explicit VortexHost(const Manifold *baseManifold = nullptr);
  ~VortexHost() = default;

  VortexHost(const VortexHost &)            = delete;
  VortexHost &operator=(const VortexHost &) = delete;
  VortexHost(VortexHost &&)                 = delete;
  VortexHost &operator=(VortexHost &&)      = delete;

  // -- Binding & Overlay Lifecycle --------------------------------------------
  void bindManifold(const Manifold *baseManifold);
  void bindStore(xanadu::Store *store);

  [[nodiscard]] const ArenaManifold &arena() const noexcept { return arena_; }
  [[nodiscard]] ArenaManifold &arena() noexcept { return arena_; }
  [[nodiscard]] const VortexCore &core() const noexcept { return core_; }
  [[nodiscard]] VortexCore &core() noexcept { return core_; }
  [[nodiscard]] const VortexVM &vm() const noexcept { return vm_; }
  [[nodiscard]] VortexVM &vm() noexcept { return vm_; }
  [[nodiscard]] const VortexStdLib &stdlib() const noexcept { return stdlib_; }
  [[nodiscard]] VortexStdLib &stdlib() noexcept { return stdlib_; }
  [[nodiscard]] const xanadu::vql::VQLEngine &vqlEngine() const noexcept {
    return vqlEngine_;
  }
  [[nodiscard]] xanadu::vql::VQLEngine &vqlEngine() noexcept {
    return vqlEngine_;
  }
  [[nodiscard]] const xanadu::vql::VQLCompiler &vqlCompiler() const noexcept {
    return vqlCompiler_;
  }
  [[nodiscard]] xanadu::vql::VQLCompiler &vqlCompiler() noexcept {
    return vqlCompiler_;
  }
  [[nodiscard]] const xanadu::vpl::VPLEngine &vplEngine() const noexcept {
    return vplEngine_;
  }
  [[nodiscard]] xanadu::vpl::VPLEngine &vplEngine() noexcept {
    return vplEngine_;
  }

  void setAppActionDelegate(AppActionDelegate delegate) {
    appActionDelegate_ = std::move(delegate);
  }
  [[nodiscard]] const AppActionDelegate &appActionDelegate() const noexcept {
    return appActionDelegate_;
  }

  // -- Configuration ----------------------------------------------------------
  void setConfig(VortexHostConfig config) noexcept {
    config_ = std::move(config);
  }
  [[nodiscard]] const VortexHostConfig &config() const noexcept {
    return config_;
  }
  void loadConfigFromStore(const xanadu::Store &store);

  // -- Process Scheduler & Services -------------------------------------------
  /// Steps all active cursors along d.cursors within the cycle budget.
  std::size_t
  stepScheduler(std::optional<std::size_t> cycleBudget = std::nullopt);

  /// Daemon cursor for background reachability GC.
  [[nodiscard]] CellRef gcCursor() const noexcept { return gcCursor_; }

  /// Triggers an immediate topological GC sweep and compaction pass.
  std::size_t triggerGarbageCollection();

  // -- Keymap Action Dispatching ----------------------------------------------
  /**
   * @brief Dispatches a named action through native Vortex standard library
   *        routines or registered VQL macros.
   *
   * @param actionName The action identifier (e.g. "step-x-pos", "insert-cell").
   * @param focusCell The currently focused cell in the visualizer.
   * @param axes The active X, Y, Z view axis bindings.
   * @param newFocusOut Out parameter populated with updated focus cell if
   * moved.
   * @return true if the action was handled by a Vortex routine or macro.
   */
  bool dispatchAction(std::string_view actionName, CellRef focusCell,
                      ViewAxisBinding &axes, CellRef &newFocusOut);
  bool dispatchAction(std::string_view actionName, CellRef focusCell,
                      const ViewAxisBinding &axes, CellRef &newFocusOut);
  void setView(ViewAxisBinding &axes, std::string_view dimX,
               std::string_view dimY, std::string_view dimZ);

  // -- Sovereign Store Library Packaging (Zero YAML) --------------------------
  bool exportLibrary(std::string_view moduleName,
                     const std::string &destinationPath) const;
  bool exportStandardLibrary(const std::string &destinationPath) const;
  bool importLibrary(const std::string &sourcePath);
  bool importLibrary(const xanadu::Store &store);

  [[nodiscard]] bool hasCustomAction(std::string_view actionName) const;

  void registerActionRoutine(std::string_view actionName, CellRef routineOp);
  void registerActionMacro(std::string_view actionName,
                           std::string_view vqlExpr);

  // -- Opcode & Library Palette -----------------------------------------------
  [[nodiscard]] std::vector<std::string> availableModules() const;
  [[nodiscard]] std::vector<std::string>
  symbolsInModule(std::string_view modulePath) const;

  /**
   * @brief Clones a symbol or opcode from d.stdlib into targetCell along
   * +d.spin or a specified dimension.
   */
  CellRef cloneSymbolToChain(std::string_view symbolPath, CellRef targetCell);

  // -- Stage 2: In-App VQL Translation & Chain Attachment ---------------------
  /**
   * @brief Compiles a VQL expression and attaches the generated Vortex opcodes
   *        along attachDimension (defaulting to +d.spin) from targetCell.
   */
  xanadu::vql::CompilationResult
  compileAndAttachVQL(std::string_view queryString, CellRef targetCell,
                      std::string_view attachDimension = "d.spin",
                      DimVector dir = DimVector::POS, bool spawnThread = false);

  /**
   * @brief Promotes a compiled opcode subgraph rooted at entryOpcode into a
   * Store, and links it to persistentTarget along attachDimension in direction
   * dir.
   */
  std::optional<zigzag::Promoted>
  promoteAndAttachToStore(CellRef entryOpcode, CellRef persistentTarget,
                          std::string_view attachDimension, DimVector dir,
                          xanadu::Store &store,
                          const xanadu::MicroversionId &parent);

  // -- Stage 3: Interactive VQL Execution & Macro Persistence -----------------
  struct ScriptResult {
    bool success{false};
    std::string message;
    std::vector<CellRef> affectedCells;
  };

  /**
   * @brief Executes an arbitrary VQL query or weave statement.
   */
  std::vector<CellRef>
  executeVQL(std::string_view queryOrWeave,
             const std::vector<CellRef> &contextCells = {});

  /**
   * @brief Evaluates a VQL path relative to currentFocus (or ##), returning the
   * destination cell if reached.
   */
  std::optional<CellRef> navigatePath(std::string_view pathExpr,
                                      CellRef currentFocus);

  /**
   * @brief Runs an arbitrary VQL script or weave block, promoting created cells
   * into store if provided.
   */
  ScriptResult executeScript(std::string_view script,
                             CellRef contextCell  = noCell,
                             xanadu::Store *store = nullptr);

  /**
   * @brief Evaluates an arbitrary VPL expression and formats the array view.
   */
  ScriptResult executeVPL(std::string_view expr);

  /**
   * @brief Solves a first-order logic goal query using VProlog and Vlog.
   */
  std::vector<LogicSolution> solveLogic(std::string_view goalQuery,
                                        std::size_t maxSolutions = 50);

  /**
   * @brief Persists a named macro into the active sovereign Store
   * (system://keymap).
   */
  bool defineMacro(std::string_view name, std::string_view vqlExpr,
                   xanadu::Store *persistStore = nullptr);

  /**
   * @brief Loads all macro definitions ("macro.*") from system://keymap.
   */
  void loadMacrosFromStore(const xanadu::Store &keymapStore);

  /**
   * @brief Saves a macro definition to keymapStore with microversion tracking.
   */
  xanadu::MicroversionId saveMacroToStore(std::string_view macroName,
                                          std::string_view vqlExpr,
                                          std::string_view keyBinding,
                                          xanadu::Store &keymapStore);

  [[nodiscard]] std::optional<std::string>
  getMacro(std::string_view name) const;
  [[nodiscard]] std::vector<std::string> listMacros() const;

private:
  void initHostServices();
  DimRef resolveDimRef(std::string_view dimName);

  ArenaManifold arena_;
  VortexCore core_;
  VortexVM vm_;
  VortexStdLib stdlib_;
  xanadu::vql::VQLEngine vqlEngine_;
  xanadu::vql::VQLCompiler vqlCompiler_;
  xanadu::vpl::VPLEngine vplEngine_;

  AppActionDelegate appActionDelegate_{};
  xanadu::Store *boundStore_{nullptr};
  VortexHostConfig config_{};
  CellRef gcCursor_{noCell};
  std::size_t frameCount_{0};

  std::unordered_map<std::string, CellRef> customActionRoutines_;
  std::unordered_map<std::string, std::string> macroRegistry_;
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_HOST_HPP

/**
 * @file vortex_host.cpp
 * @brief Implementation of the VortexHost standardized public interface.
 */
#include "common/xanadu/vortex/vortex_host.hpp"

#include <algorithm>
#include <format>
#include <iostream>
#include <thread>

#include <gleditor/logging.hpp>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace zigzag::vortex {

namespace {
/// Whether @p store's keymap already has its group hierarchy at @p version:
/// a d.groups rank hanging off home. Without one it needs a genesis.
bool hasKeymapGroups(xanadu::Store &store,
                     const xanadu::MicroversionId &version) {
  const auto manifold = store.rebuildManifold(version);
  return DimensionRegistry::instance()
      .get(store, xanadu::kDimGroups)
      .and_then([&](const DimRef groups) {
        return step(manifold, store.homeCell(), groups);
      })
      .has_value();
}
} // namespace

VortexHostConfig VortexHostConfig::fromStore(const xanadu::Store &store) {
  VortexHostConfig cfg{};
  const auto model = xanadu::SystemStoreModel::fromStore(store);
  if (!model.isValid()) {
    return cfg;
  }

  const auto budgetVals = xanadu::getSetting(store, "vortex.cycle_budget");
  if (!budgetVals.empty() &&
      std::holds_alternative<std::int64_t>(budgetVals[0])) {
    cfg.schedulerCycleBudget =
        static_cast<std::size_t>(std::get<std::int64_t>(budgetVals[0]));
  }

  const auto gcVals = xanadu::getSetting(store, "vortex.gc_interval_frames");
  if (!gcVals.empty() && std::holds_alternative<std::int64_t>(gcVals[0])) {
    cfg.gcIntervalFrames =
        static_cast<std::size_t>(std::get<std::int64_t>(gcVals[0]));
  }

  const auto promptVals =
      xanadu::getSetting(store, "vortex.command_prompt_hotkey");
  if (!promptVals.empty() &&
      std::holds_alternative<std::string>(promptVals[0])) {
    cfg.commandPromptHotkey = std::get<std::string>(promptVals[0]);
  }

  const auto palVals = xanadu::getSetting(store, "vortex.palette_hotkey");
  if (!palVals.empty() && std::holds_alternative<std::string>(palVals[0])) {
    cfg.paletteHotkey = std::get<std::string>(palVals[0]);
  }

  const auto bundleVals = xanadu::getSetting(store, "vortex.default_bundle");
  if (!bundleVals.empty() &&
      std::holds_alternative<std::string>(bundleVals[0])) {
    cfg.defaultBundle = std::get<std::string>(bundleVals[0]);
  }

  const auto workerVals =
      xanadu::getSetting(store, xanadu::settings::kVortexWorkerThreads);
  if (!workerVals.empty() &&
      std::holds_alternative<std::int64_t>(workerVals[0])) {
    cfg.workerThreads =
        static_cast<std::size_t>(std::get<std::int64_t>(workerVals[0]));
  }

  const auto thresholdVals =
      xanadu::getSetting(store, xanadu::settings::kVortexParallelThreshold);
  if (!thresholdVals.empty() &&
      std::holds_alternative<std::int64_t>(thresholdVals[0])) {
    cfg.parallelThreshold =
        static_cast<std::size_t>(std::get<std::int64_t>(thresholdVals[0]));
  }

  const auto orParVals =
      xanadu::getSetting(store, xanadu::settings::kVortexOrParallelSearch);
  if (!orParVals.empty() && std::holds_alternative<bool>(orParVals[0])) {
    cfg.orParallelSearch = std::get<bool>(orParVals[0]);
  }

  return cfg;
}

VortexHost::VortexHost(const Manifold *baseManifold)
    : baseManifold_(baseManifold), primary_(baseManifold),
      vqlEngine_(primary_.core), vqlCompiler_(primary_.core, primary_.vm),
      vplEngine_(primary_.core) {
  initHostServices();
}

WorkerContext &VortexHost::makeWorkerContext() {
  // Stage 1 (vortex-multithreading-study.md §3.3): hold an additional context
  // over the SAME shared base. Single-threaded still -- this starts no thread;
  // it only lets the host own more than one execution context. unique_ptr in a
  // deque keeps the returned reference stable as more are added.
  extraContexts_.push_back(std::make_unique<WorkerContext>(baseManifold_));
  extraContexts_.back()->stdlib.bootstrap();
  return *extraContexts_.back();
}

void VortexHost::initHostServices() {
  primary_.stdlib.bootstrap();

  if (const auto sweepOp = primary_.stdlib.resolve("std:gc/sweep")) {
    gcCursor_ = primary_.vm.spawnCursor(*sweepOp, "sys:gc");
  }

  // Register native Vortex routines for keymap actions
  auto regPair = [&](std::string_view legacy, std::string_view canonical) {
    if (const auto op = primary_.stdlib.resolve(canonical)) {
      registerActionRoutine(legacy, *op);
      registerActionRoutine(canonical, *op);
    }
  };
  regPair("swap-xy", "std:ui/swap_axes");
  regPair("cycle-dims-forward", "std:ui/cycle_dims_forward");
  regPair("cycle-dims-backward", "std:ui/cycle_dims_backward");
  regPair("bundle-execution", "std:ui/bundle_execution");
  regPair("bundle-scope", "std:ui/bundle_scope");
  regPair("bundle-contract", "std:ui/bundle_contract");
  regPair("bundle-logic", "std:ui/bundle_logic");
  regPair("bundle-stdlib", "std:ui/bundle_stdlib");
  regPair("hop-head", "std:nav/hop_head");
  regPair("hop-tail", "std:nav/hop_tail");
  regPair("jump-home", "std:nav/jump_home");
  regPair("duplicate-focus-cell", "std:zigzag/duplicate");

  // Stage 2: build the data-parallel worker pool from config (defaults until a
  // store is bound) and attach it to the primary stdlib.
  applyParallelConfig();
}

void VortexHost::bindManifold(const Manifold *baseManifold) {
  // Reassign the primary arena in place: primary_.core/vm/stdlib hold
  // references into primary_.arena, and move-assigning the arena replaces its
  // contents while keeping those references valid. Record the base so any
  // worker contexts made afterwards overlay the same one (Stage 1 §3.3).
  baseManifold_  = baseManifold;
  primary_.arena = ArenaManifold(baseManifold);
  extraContexts_.clear();
  initHostServices();
}

void VortexHost::bindStore(xanadu::Store *store) {
  boundStore_ = store;
  primary_.stdlib.setBoundStore(store);
  vplEngine_.setStore(store);
  if (boundStore_) {
    loadConfigFromStore(*boundStore_);
  }
}

void VortexHost::loadConfigFromStore(const xanadu::Store &store) {
  config_ = VortexHostConfig::fromStore(store);
  applyParallelConfig();
}

void VortexHost::applyParallelConfig() {
  // Resolve the worker count: 0 => probe hardware_concurrency, else the
  // configured value; a pool of 1 starts no threads (caller-only). Build the
  // pool and attach it to the primary stdlib with the configured threshold.
  std::size_t threads = config_.workerThreads;
  if (threads == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    threads           = (hw == 0) ? 1 : static_cast<std::size_t>(hw);
  }
  workerPool_ =
      std::make_unique<VortexWorkerPool>(static_cast<std::uint32_t>(threads));
  primary_.stdlib.setWorkerPool(workerPool_.get(), config_.parallelThreshold);
}

std::size_t VortexHost::stepScheduler(std::optional<std::size_t> cycleBudget) {
  const std::size_t budget = cycleBudget.value_or(config_.schedulerCycleBudget);
  std::size_t executed     = 0;
  for (std::size_t i = 0; i < budget; ++i) {
    std::size_t stepCount = primary_.vm.stepScheduler();
    if (stepCount == 0) {
      break;
    }
    executed += stepCount;
  }

  ++frameCount_;
  if (frameCount_ >= config_.gcIntervalFrames) {
    frameCount_ = 0;
    triggerGarbageCollection();
  }

  return executed;
}

std::size_t VortexHost::triggerGarbageCollection() {
  return primary_.stdlib.gcSweep();
}

DimRef VortexHost::resolveDimRef(std::string_view dimName) {
  if (dimName == "d.spin") return primary_.core.dims().spin;
  if (dimName == "d.grab") return primary_.core.dims().grab;
  if (dimName == "d.step") return primary_.core.dims().step;
  if (dimName == "d.contract") return primary_.core.dims().contract;
  if (dimName == "d.clone") return primary_.core.dims().clone;
  if (dimName == "d.cursors") return primary_.core.dims().cursors;
  if (dimName == "d.cache") return primary_.core.dims().cache;
  if (dimName == "d.vars") return primary_.core.dims().vars;
  if (dimName == "d.values") return primary_.core.dims().values;
  if (dimName == "d.pinning-cursors")
    return primary_.core.dims().pinningCursors;
  if (dimName == "d.name") return primary_.core.dims().name;
  if (dimName == "d.stdlib") return primary_.core.dims().stdlib;
  if (dimName == "d.clause") return primary_.core.dims().clause;

  return primary_.core.mintDimension(dimName);
}

bool VortexHost::dispatchAction(const std::string_view actionName) {
  ViewAxisBinding axes{};
  CellRef newFocusOut{};
  return dispatchAction(actionName, 0, axes, newFocusOut);
}

bool VortexHost::dispatchAction(std::string_view actionName, CellRef focusCell,
                                const ViewAxisBinding &axes,
                                CellRef &newFocusOut) {
  ViewAxisBinding mutableAxes = axes;
  return dispatchAction(actionName, focusCell, mutableAxes, newFocusOut);
}

bool VortexHost::dispatchAction(std::string_view actionName, CellRef focusCell,
                                ViewAxisBinding &axes, CellRef &newFocusOut) {
  newFocusOut                      = focusCell;
  const std::string_view canonical = xanadu::canonicalKeymapAction(actionName);

  auto macroIt = macroRegistry_.find(std::string(actionName));
  if (macroIt == macroRegistry_.end() && canonical != actionName) {
    macroIt = macroRegistry_.find(std::string(canonical));
  }
  if (macroIt != macroRegistry_.end()) {
    auto target = navigatePath(macroIt->second, focusCell);
    if (target.has_value()) {
      if (const auto cell = gleditor::fromSentinel<zigzag::noCell>(*target)) {
        newFocusOut = *cell;
        return true;
      }
    }
    auto res = executeScript(macroIt->second, focusCell);
    if (res.success && !res.affectedCells.empty()) {
      newFocusOut = res.affectedCells.back();
    }
    return res.success;
  }

  auto customIt = customActionRoutines_.find(std::string(actionName));
  if (customIt == customActionRoutines_.end() && canonical != actionName) {
    customIt = customActionRoutines_.find(std::string(canonical));
  }
  if (customIt != customActionRoutines_.end()) {
    if (const auto routineCell =
            gleditor::fromSentinel<zigzag::noCell>(customIt->second)) {
      CellRef cursor = primary_.vm.spawnCursor(*routineCell, actionName);
      if (const auto ran = primary_.vm.run(cursor, 1000); !ran.success) {
        GLEDITOR_LOG_WARN("vortex.host", "action {} failed: {}", actionName,
                          ran.errorMessage);
      }
    }
  }

  // Application action delegate hook (e.g. apps/xudu, apps/zigzag)
  if (appActionDelegate_ &&
      appActionDelegate_(canonical, focusCell, axes, newFocusOut)) {
    return true;
  }

  // Navigation actions
  if (actionName == "step-x-pos" || canonical == "std:nav/step_x_pos") {
    newFocusOut = primary_.stdlib.zzStep(
        focusCell, resolveDimRef(axes.x_dimension), DimVector::POS);
    return true;
  }
  if (actionName == "step-x-neg" || canonical == "std:nav/step_x_neg") {
    newFocusOut = primary_.stdlib.zzStep(
        focusCell, resolveDimRef(axes.x_dimension), DimVector::NEG);
    return true;
  }
  if (actionName == "step-y-pos" || canonical == "std:nav/step_y_pos") {
    newFocusOut = primary_.stdlib.zzStep(
        focusCell, resolveDimRef(axes.y_dimension), DimVector::POS);
    return true;
  }
  if (actionName == "step-y-neg" || canonical == "std:nav/step_y_neg") {
    newFocusOut = primary_.stdlib.zzStep(
        focusCell, resolveDimRef(axes.y_dimension), DimVector::NEG);
    return true;
  }
  if (actionName == "step-z-pos" || canonical == "std:nav/step_z_pos") {
    newFocusOut = primary_.stdlib.zzStep(
        focusCell, resolveDimRef(axes.z_dimension), DimVector::POS);
    return true;
  }
  if (actionName == "step-z-neg" || canonical == "std:nav/step_z_neg") {
    newFocusOut = primary_.stdlib.zzStep(
        focusCell, resolveDimRef(axes.z_dimension), DimVector::NEG);
    return true;
  }
  if (actionName == "hop-head" || canonical == "std:nav/hop_head") {
    newFocusOut =
        primary_.stdlib.hopHead(focusCell, resolveDimRef(axes.x_dimension));
    return true;
  }
  if (actionName == "hop-tail" || canonical == "std:nav/hop_tail") {
    newFocusOut =
        primary_.stdlib.hopTail(focusCell, resolveDimRef(axes.x_dimension));
    return true;
  }
  if (actionName == "jump-home" || canonical == "std:nav/jump_home") {
    newFocusOut = primary_.stdlib.jumpHome();
    return true;
  }

  // Editing actions
  if (actionName == "insert-cell-x-pos" ||
      canonical == "std:zigzag/insert_cell_x_pos") {
    newFocusOut = primary_.stdlib.zzInsert(
        focusCell, resolveDimRef(axes.x_dimension), DimVector::POS, "New Cell");
    return true;
  }
  if (actionName == "insert-cell-x-neg" ||
      canonical == "std:zigzag/insert_cell_x_neg") {
    newFocusOut = primary_.stdlib.zzInsert(
        focusCell, resolveDimRef(axes.x_dimension), DimVector::NEG, "New Cell");
    return true;
  }
  if (actionName == "insert-cell-y-pos" ||
      canonical == "std:zigzag/insert_cell_y_pos") {
    newFocusOut = primary_.stdlib.zzInsert(
        focusCell, resolveDimRef(axes.y_dimension), DimVector::POS, "New Cell");
    return true;
  }
  if (actionName == "insert-cell-y-neg" ||
      canonical == "std:zigzag/insert_cell_y_neg") {
    newFocusOut = primary_.stdlib.zzInsert(
        focusCell, resolveDimRef(axes.y_dimension), DimVector::NEG, "New Cell");
    return true;
  }
  if (actionName == "unlink-x-pos" || canonical == "std:zigzag/unlink_x_pos") {
    primary_.stdlib.zzUnlink(focusCell, resolveDimRef(axes.x_dimension),
                             DimVector::POS);
    return true;
  }
  if (actionName == "unlink-x-neg" || canonical == "std:zigzag/unlink_x_neg") {
    primary_.stdlib.zzUnlink(focusCell, resolveDimRef(axes.x_dimension),
                             DimVector::NEG);
    return true;
  }
  if (actionName == "delete-focus-cell" ||
      canonical == "std:zigzag/delete_focus_cell" ||
      canonical == "std:zigzag/delete_focus_cell_bksp") {
    primary_.stdlib.zzDelete(focusCell);
    return true;
  }
  if (actionName == "duplicate-focus-cell" ||
      canonical == "std:zigzag/duplicate") {
    newFocusOut = primary_.stdlib.zzDuplicate(focusCell);
    if (boundStore_ && newFocusOut != noCell &&
        zigzag::isEphemeral(newFocusOut)) {
      zigzag::PromotionBudget budget{.maxOps = 1000};
      auto promoted =
          zigzag::promote(*boundStore_, boundStore_->primaryCurrentVersion(),
                          primary_.arena, newFocusOut, budget);
      if (!promoted || promoted->cells.empty()) {
        GLEDITOR_LOG_WARN("vortex.edit",
                          "duplicate promotion refused for arena cell {}",
                          newFocusOut);
        newFocusOut = noCell;
        return false;
      }
      boundStore_->repointCurrentVersion(promoted->version);
      // promote() lists newly minted cells in discovery order, root first.
      newFocusOut = promoted->cells.front();
      GLEDITOR_LOG_DEBUG("vortex.edit", "duplicate promoted to {}",
                         newFocusOut);
    }
    return true;
  }

  // UI actions
  if (actionName.starts_with("view ") ||
      actionName.starts_with("std:ui/view ") ||
      canonical.starts_with("std:ui/view ")) {
    std::string_view target = actionName;
    if (!target.starts_with("view ") && !target.starts_with("std:ui/view ")) {
      target = canonical;
    }
    std::string_view rest = target.substr(target.find(' ') + 1);
    std::istringstream iss{std::string(rest)};
    std::string dx;
    std::string dy;
    std::string dz;
    iss >> dx >> dy >> dz;
    setView(axes, dx, dy, dz);
    return true;
  }
  if (actionName == "swap-xy" || canonical == "std:ui/swap_axes") {
    primary_.stdlib.swapAxes(axes);
    return true;
  }
  if (actionName == "cycle-dims-forward" ||
      canonical == "std:ui/cycle_dims_forward") {
    primary_.stdlib.cycleDims(axes, true);
    return true;
  }
  if (actionName == "cycle-dims-backward" ||
      canonical == "std:ui/cycle_dims_backward") {
    primary_.stdlib.cycleDims(axes, false);
    return true;
  }
  if (actionName == "bundle-execution" ||
      canonical == "std:ui/bundle_execution") {
    primary_.stdlib.applyBundle(axes, DimensionBundle::Execution);
    return true;
  }
  if (actionName == "bundle-scope" || canonical == "std:ui/bundle_scope") {
    primary_.stdlib.applyBundle(axes, DimensionBundle::Scope);
    return true;
  }
  if (actionName == "bundle-contract" ||
      canonical == "std:ui/bundle_contract") {
    primary_.stdlib.applyBundle(axes, DimensionBundle::Contract);
    return true;
  }
  if (actionName == "bundle-logic" || canonical == "std:ui/bundle_logic") {
    primary_.stdlib.applyBundle(axes, DimensionBundle::Logic);
    return true;
  }
  if (actionName == "bundle-stdlib" || canonical == "std:ui/bundle_stdlib") {
    primary_.stdlib.applyBundle(axes, DimensionBundle::Stdlib);
    return true;
  }
  if (actionName == "bundle-cycle" || canonical == "std:ui/bundle_cycle") {
    DimensionBundle next = DimensionBundle::Execution;
    if (axes.x_dimension == "d.spin" && axes.y_dimension == "d.step") {
      next = DimensionBundle::Scope;
    } else if (axes.x_dimension == "d.lexical" &&
               axes.y_dimension == "d.dynamic") {
      next = DimensionBundle::Contract;
    } else if (axes.x_dimension == "d.require" &&
               axes.y_dimension == "d.ensure") {
      next = DimensionBundle::Logic;
    } else if (axes.x_dimension == "d.clause" &&
               axes.y_dimension == "d.predicate") {
      next = DimensionBundle::Stdlib;
    }
    primary_.stdlib.applyBundle(axes, next);
    return true;
  }

  // Stdlib fallback: check if standard library has a registered routine cell
  const auto stdlibOp = primary_.stdlib.resolve(canonical).or_else([&] {
    return canonical != actionName ? primary_.stdlib.resolve(actionName)
                                   : std::nullopt;
  });
  if (stdlibOp) {
    CellRef cursor = primary_.vm.spawnCursor(*stdlibOp, canonical);
    if (const auto ran = primary_.vm.run(cursor, 1000); !ran.success) {
      GLEDITOR_LOG_WARN("vortex.host", "action {} failed: {}", canonical,
                        ran.errorMessage);
    }
    return true;
  }

  return false;
}

void VortexHost::setView(ViewAxisBinding &axes, std::string_view dimX,
                         std::string_view dimY, std::string_view dimZ) {
  primary_.stdlib.setView(axes, dimX, dimY, dimZ);
}

bool VortexHost::exportLibrary(std::string_view moduleName,
                               const std::string &destinationPath) const {
  xanadu::Store store;
  if (!primary_.stdlib.exportModuleToStore(moduleName, store)) {
    return false;
  }
  try {
    store.save(destinationPath);
    return true;
  } catch (...) {
    return false;
  }
}

bool VortexHost::exportStandardLibrary(
    const std::string &destinationPath) const {
  xanadu::Store store;
  if (!primary_.stdlib.exportStandardLibraryToStore(store)) {
    return false;
  }
  try {
    store.save(destinationPath);
    return true;
  } catch (...) {
    return false;
  }
}

bool VortexHost::importLibrary(const std::string &sourcePath) {
  try {
    xanadu::Store store;
    store.load(sourcePath);
    return importLibrary(store);
  } catch (...) {
    return false;
  }
}

bool VortexHost::importLibrary(const xanadu::Store &store) {
  CellRef res = primary_.stdlib.importModuleFromStore(store);
  return res != noCell;
}

bool VortexHost::hasCustomAction(std::string_view actionName) const {
  return customActionRoutines_.contains(std::string(actionName)) ||
         macroRegistry_.contains(std::string(actionName));
}

void VortexHost::registerActionRoutine(std::string_view actionName,
                                       CellRef routineOp) {
  customActionRoutines_[std::string(actionName)] = routineOp;
}

void VortexHost::registerActionMacro(std::string_view actionName,
                                     std::string_view vqlExpr) {
  macroRegistry_[std::string(actionName)] = std::string(vqlExpr);
}

std::vector<std::string> VortexHost::availableModules() const {
  return primary_.stdlib.modules();
}

std::vector<std::string>
VortexHost::symbolsInModule(std::string_view modulePath) const {
  return primary_.stdlib.symbolsInModule(modulePath);
}

CellRef VortexHost::cloneSymbolToChain(std::string_view symbolPath,
                                       CellRef targetCell) {
  if (symbolPath.starts_with("#")) {
    CellRef op = primary_.arena.makeCell(symbolPath);
    if (targetCell != noCell && primary_.arena.contains(targetCell)) {
      primary_.core.link(targetCell, primary_.core.dims().step, DimVector::POS,
                         op);
    }
    return op;
  }
  return primary_.stdlib.resolve(symbolPath)
      .transform([&](const CellRef sym) {
        return primary_.stdlib.zzCloneToChain(sym, targetCell);
      })
      .value_or(noCell);
}

xanadu::vql::CompilationResult VortexHost::compileAndAttachVQL(
    std::string_view queryString, CellRef targetCell,
    std::string_view attachDimension, DimVector dir, bool spawnThread) {
  xanadu::vql::CompilationOptions options;
  options.targetLibrary = false;
  auto result           = vqlCompiler_.compile(queryString, options);
  if (!result.success || result.entryOpcode == noCell) {
    return result;
  }

  if (targetCell != noCell && primary_.arena.contains(targetCell)) {
    const DimRef dim = resolveDimRef(attachDimension);
    primary_.core.link(targetCell, dim, dir, result.entryOpcode);
  }

  if (spawnThread) {
    primary_.vm.spawnCursor(result.entryOpcode, "vql_exec");
  }

  return result;
}

std::optional<zigzag::Promoted> VortexHost::promoteAndAttachToStore(
    CellRef entryOpcode, CellRef persistentTarget,
    std::string_view attachDimension, DimVector dir, xanadu::Store &store,
    const xanadu::MicroversionId &parent) {
  if (entryOpcode == noCell || !primary_.arena.contains(entryOpcode)) {
    return std::nullopt;
  }

  auto promoted = zigzag::promote(store, parent, primary_.arena, entryOpcode);
  if (!promoted || promoted->cells.empty()) {
    return std::nullopt;
  }

  if (persistentTarget != noCell && persistentTarget != zigzag::noCell) {
    auto manifold     = store.rebuildManifold(promoted->version);
    const auto attach = zigzag::DimensionRegistry::instance().getOrCreate(
        store, promoted->version, manifold, attachDimension);
    if (!attach) {
      // The program is persisted either way; with no dimension to hang it
      // on it stays unattached rather than linked along noCell, which is an
      // operation every fold would refuse.
      GLEDITOR_LOG_WARN("vortex.edit", "cannot attach promoted program: {}",
                        zigzag::toString(attach.error()));
      return promoted;
    }
    const DimRef dimRef = *attach;
    manifold.advanceOrRefold(store, promoted->version);
    const CellRef persistentEntryOp = promoted->cells.front();
    promoted->version = store.setLink(promoted->version, persistentTarget,
                                      dimRef, dir, persistentEntryOp);
  }

  return promoted;
}

std::vector<CellRef>
VortexHost::executeVQL(std::string_view queryOrWeave,
                       const std::vector<CellRef> &contextCells) {
  try {
    xanadu::vql::Parser parser(queryOrWeave);
    auto expr = parser.parseQuery();
    if (std::holds_alternative<xanadu::vql::PathExpression>(expr.expr)) {
      return vqlEngine_.evaluatePath(
          std::get<xanadu::vql::PathExpression>(expr.expr), contextCells);
    }
    return vqlEngine_.execute(expr);
  } catch (const std::exception &err) {
    std::cerr << "VortexHost::executeVQL error: " << err.what() << "\n";
    return {};
  }
}

std::vector<std::vector<std::string>> VortexHost::executeVQLBatchParallel(
    const std::vector<std::string> &queries,
    gleditor::cpp26::function_ref<void(WorkerContext &, std::size_t)> prepare) {
  std::vector<std::vector<std::string>> results(queries.size());
  if (queries.empty()) {
    return results;
  }

  // One worker context per query, each overlaying the shared frozen base.
  // Building them up front (serially) keeps makeWorkerContext single-threaded;
  // the parallel phase only touches each context's own arena.
  std::vector<WorkerContext *> contexts;
  contexts.reserve(queries.size());
  for (std::size_t i = 0; i < queries.size(); ++i) {
    contexts.push_back(&makeWorkerContext());
  }

  // Runs query i on context i: populate its ephemeral cells, then evaluate the
  // query on a VQLEngine bound to that context's core, rendering results to
  // context-independent strings so the batch is comparable and deterministic.
  const auto runOne = [&](std::size_t i) {
    WorkerContext &ctx = *contexts[i];
    prepare(ctx, i);
    xanadu::vql::VQLEngine engine(ctx.core);
    std::vector<CellRef> cells;
    try {
      xanadu::vql::Parser parser(queries[i]);
      auto expr = parser.parseQuery();
      if (std::holds_alternative<xanadu::vql::PathExpression>(expr.expr)) {
        cells = engine.evaluatePath(
            std::get<xanadu::vql::PathExpression>(expr.expr), {});
      } else {
        cells = engine.execute(expr);
      }
    } catch (const std::exception &) {
      cells.clear();
    }
    std::vector<std::string> rendered;
    rendered.reserve(cells.size());
    for (const CellRef c : cells) {
      // Render each result cell to a context-independent string so the batch
      // is comparable across worker arenas and deterministic.
      const CellValue v = ctx.core.render(c);
      rendered.push_back(std::visit(
          [](const auto &held) -> std::string {
            using T = std::remove_cvref_t<decltype(held)>;
            if constexpr (std::is_same_v<T, std::string>) {
              return held;
            } else if constexpr (std::is_same_v<T, bool>) {
              return held ? "true" : "false";
            } else {
              return std::to_string(held);
            }
          },
          v));
    }
    results[i] = std::move(rendered);
  };

  if (workerPool_ != nullptr && queries.size() > 1 &&
      workerPool_->parallelism() > 1) {
    workerPool_->run(static_cast<std::uint32_t>(queries.size()),
                     [&](std::uint32_t i) { runOne(i); });
  } else {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      runOne(i);
    }
  }
  return results;
}

std::optional<CellRef> VortexHost::navigatePath(std::string_view pathExpr,
                                                CellRef currentFocus) {
  try {
    std::string query(pathExpr);
    while (!query.empty() &&
           std::isspace(static_cast<unsigned char>(query.front()))) {
      query.erase(query.begin());
    }
    while (!query.empty() &&
           std::isspace(static_cast<unsigned char>(query.back()))) {
      query.pop_back();
    }
    if (query.empty()) {
      return std::nullopt;
    }

    xanadu::vql::Parser parser(query);
    auto expr = parser.parseQuery();
    std::vector<CellRef> results;
    if (std::holds_alternative<xanadu::vql::PathExpression>(expr.expr)) {
      std::vector<CellRef> ctx;
      if (currentFocus != noCell) {
        ctx.push_back(currentFocus);
      }
      results = vqlEngine_.evaluatePath(
          std::get<xanadu::vql::PathExpression>(expr.expr), ctx);
    } else {
      results = vqlEngine_.execute(expr);
    }
    if (!results.empty() && results.front() != noCell) {
      return results.front();
    }
  } catch (const std::exception &err) {
    std::cerr << "VortexHost::navigatePath error: " << err.what() << "\n";
  }
  return std::nullopt;
}

VortexHost::ScriptResult VortexHost::executeScript(std::string_view script,
                                                   CellRef contextCell,
                                                   xanadu::Store *store) {
  ScriptResult res;
  try {
    std::string_view trimmed = script;
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.front()))) {
      trimmed.remove_prefix(1);
    }
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.back()))) {
      trimmed.remove_suffix(1);
    }
    if (trimmed.empty()) {
      res.message = "Empty script";
      return res;
    }

    if (trimmed.starts_with(')') || trimmed.starts_with(":vpl ") ||
        trimmed.starts_with("vpl:")) {
      res = executeVPL(trimmed);
      if (store && !res.affectedCells.empty()) {
        for (CellRef c : res.affectedCells) {
          if (zigzag::isEphemeral(c)) {
            if (!zigzag::promote(*store, store->primaryCurrentVersion(),
                                 primary_.arena, c)) {
              // Past the promotion budget, or not a cell of this arena: the
              // result stays in the arena, and the store does not have it.
              GLEDITOR_LOG_WARN("vortex.edit",
                                "cell {} was not written to the store", c);
            }
          }
        }
      }
      return res;
    }

    if (trimmed.starts_with(":logic ") || trimmed.starts_with(":query ") ||
        trimmed.starts_with(":solve ") || trimmed.starts_with("?- ")) {
      std::string_view q = trimmed;
      if (trimmed.starts_with(":logic ") || trimmed.starts_with(":query ") ||
          trimmed.starts_with(":solve ")) {
        // All three command prefixes happen to be 7 characters; "?- " (the
        // fourth form checked above) is Prolog's own query syntax and is
        // left in place rather than stripped.
        q.remove_prefix(7);
      }
      auto solutions = solveLogic(q);
      res.success    = !solutions.empty();
      res.message =
          std::format("Logic query returned {} solution(s)", solutions.size());
      for (const auto &sol : solutions) {
        for (const auto &[name, cell] : sol.bindings) {
          res.affectedCells.push_back(cell);
        }
      }
      return res;
    }

    if (contextCell != noCell) {
      vqlEngine_.setVariable(".", contextCell);
    }
    xanadu::vql::Parser parser(trimmed);
    auto expr = parser.parseQuery();
    std::vector<CellRef> cells;
    if (std::holds_alternative<xanadu::vql::PathExpression>(expr.expr)) {
      std::vector<CellRef> ctx;
      if (contextCell != noCell) {
        ctx.push_back(contextCell);
      }
      cells = vqlEngine_.evaluatePath(
          std::get<xanadu::vql::PathExpression>(expr.expr), ctx);
    } else {
      cells = vqlEngine_.execute(expr);
    }

    res.affectedCells = cells;
    res.success       = true;
    res.message = std::format("Executed VQL ({} cells affected)", cells.size());

    if (store && !cells.empty()) {
      for (CellRef c : cells) {
        if (zigzag::isEphemeral(c)) {
          if (!zigzag::promote(*store, store->primaryCurrentVersion(),
                               primary_.arena, c)) {
            // Past the promotion budget, or not a cell of this arena: the
            // result stays in the arena, and the store does not have it.
            GLEDITOR_LOG_WARN("vortex.edit",
                              "cell {} was not written to the store", c);
          }
        }
      }
    }
  } catch (const std::exception &err) {
    res.success = false;
    res.message = std::string("Script Error: ") + err.what();
  }
  return res;
}

VortexHost::ScriptResult VortexHost::executeVPL(std::string_view expr) {
  ScriptResult res;
  try {
    std::string_view trimmed = expr;
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.front()))) {
      trimmed.remove_prefix(1);
    }
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.back()))) {
      trimmed.remove_suffix(1);
    }
    if (trimmed.starts_with(":vpl ")) {
      trimmed.remove_prefix(5);
    } else if (trimmed.starts_with("vpl:")) {
      trimmed.remove_prefix(4);
    } else if (trimmed.starts_with(')')) {
      trimmed.remove_prefix(1);
    }
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.front()))) {
      trimmed.remove_prefix(1);
    }
    if (trimmed.empty()) {
      res.message = "Empty VPL expression";
      return res;
    }

    auto view         = vplEngine_.evaluate(trimmed);
    res.affectedCells = view.collectCells(primary_.arena);
    res.success       = true;
    if (view.isScalar()) {
      if (view.isString()) {
        res.message = std::format("VPL Result: \"{}\"", view.scalarString());
      } else if (view.isFloat()) {
        res.message = std::format("VPL Result: {}", view.scalarFloat());
      } else {
        res.message = std::format("VPL Result: {}", view.scalarInt());
      }
    } else {
      auto sh              = view.shape(primary_.arena);
      std::string shapeStr = "[";
      for (std::size_t i = 0; i < sh.size(); ++i) {
        if (i > 0) {
          shapeStr += ", ";
        }
        shapeStr += std::to_string(sh[i]);
      }
      shapeStr += "]";
      res.message = std::format("VPL Result: shape {} ({} cells)", shapeStr,
                                res.affectedCells.size());
    }
  } catch (const std::exception &err) {
    res.success = false;
    res.message = std::string("VPL Error: ") + err.what();
  }
  return res;
}

std::vector<LogicSolution> VortexHost::solveLogic(std::string_view goalQuery,
                                                  std::size_t maxSolutions) {
  try {
    xanadu::vprolog::Compiler compiler(primary_.core);
    auto query = compiler.compileQuery(goalQuery);
    if (query.goals.empty()) {
      return {};
    }
    return primary_.stdlib.solveQuery(query.goals, compiler.customPredicates(),
                                      maxSolutions);
  } catch (const std::exception &err) {
    std::cerr << "VortexHost::solveLogic error: " << err.what() << "\n";
    return {};
  }
}

std::vector<std::vector<std::string>>
VortexHost::solveLogicBatchParallel(const std::vector<std::string> &goalQueries,
                                    std::size_t maxSolutions) {
  std::vector<std::vector<std::string>> results(goalQueries.size());
  if (goalQueries.empty()) {
    return results;
  }

  // Render one solution's variable bindings to a stable, context-independent
  // string so solutions compare across worker arenas: "name=value" pairs
  // sorted by variable name, joined. The cell values are rendered through the
  // owning context's core (a pure const read).
  const auto renderSolution = [](WorkerContext &ctx,
                                 const LogicSolution &sol) -> std::string {
    std::vector<std::string> parts;
    parts.reserve(sol.bindings.size());
    for (const auto &[nameCell, valueCell] : sol.bindings) {
      const auto render = [&ctx](CellRef c) -> std::string {
        if (c == noCell || !ctx.arena.contains(c)) {
          return "_";
        }
        return std::visit(
            [](const auto &held) -> std::string {
              using T = std::remove_cvref_t<decltype(held)>;
              if constexpr (std::is_same_v<T, std::string>) {
                return held;
              } else if constexpr (std::is_same_v<T, bool>) {
                return held ? "true" : "false";
              } else {
                return std::to_string(held);
              }
            },
            ctx.core.render(c));
      };
      parts.push_back(render(nameCell) + "=" + render(valueCell));
    }
    std::ranges::sort(parts);
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i != 0) {
        joined += ';';
      }
      joined += parts[i];
    }
    return joined;
  };

  // Solve query i on worker context ctx: compile the query in that context's
  // core, solve on that context's stdlib, render each solution. Everything --
  // the clause database, the arena trail, mark/release -- stays inside ctx.
  const auto runOne = [&](std::size_t i, WorkerContext &ctx) {
    std::vector<std::string> rendered;
    try {
      xanadu::vprolog::Compiler compiler(ctx.core);
      auto query = compiler.compileQuery(goalQueries[i]);
      if (query.goals.empty()) {
        results[i] = {};
        return;
      }
      auto sols = ctx.stdlib.solveQuery(
          query.goals, compiler.customPredicates(), maxSolutions);
      rendered.reserve(sols.size());
      for (const auto &sol : sols) {
        rendered.push_back(renderSolution(ctx, sol));
      }
    } catch (const std::exception &err) {
      std::cerr << "solveLogicBatchParallel error: " << err.what() << "\n";
      rendered.clear();
    }
    results[i] = std::move(rendered);
  };

  // Opt-in gate: default off runs serially on the primary context, so serial
  // vprolog behaviour is unchanged. On, with a pool, fans one query per worker
  // context; results stay in submission order regardless of completion order.
  if (config_.orParallelSearch && workerPool_ != nullptr &&
      goalQueries.size() > 1 && workerPool_->parallelism() > 1) {
    std::vector<WorkerContext *> contexts;
    contexts.reserve(goalQueries.size());
    for (std::size_t i = 0; i < goalQueries.size(); ++i) {
      contexts.push_back(&makeWorkerContext());
    }
    workerPool_->run(static_cast<std::uint32_t>(goalQueries.size()),
                     [&](std::uint32_t i) { runOne(i, *contexts[i]); });
  } else {
    for (std::size_t i = 0; i < goalQueries.size(); ++i) {
      runOne(i, primary_);
    }
  }
  return results;
}

bool VortexHost::defineMacro(std::string_view name, std::string_view vqlExpr,
                             xanadu::Store *persistStore) {
  macroRegistry_[std::string(name)] = std::string(vqlExpr);

  if (persistStore) {
    try {
      auto parent = persistStore->primaryCurrentVersion();
      if (persistStore->homeCell() == noCell) {
        parent = xanadu::initializeSystemStoreGenesis(
            *persistStore, xanadu::SystemDocKind::Keymap, parent);
      } else {
        if (!hasKeymapGroups(*persistStore, parent)) {
          parent = xanadu::initializeSystemStoreGenesis(
              *persistStore, xanadu::SystemDocKind::Keymap, parent);
        }
      }
      xanadu::SettingSpec spec{
          .name    = "macro." + std::string(name),
          .notes   = "User macro: " + std::string(name),
          .schemas = {{.expectedTypes = {"string"},
                       .defaultValues = {std::string{vqlExpr}}}}};
      parent = xanadu::ensureSetting(*persistStore, parent, spec);
      parent = xanadu::setSetting(*persistStore, parent,
                                  "macro." + std::string(name),
                                  std::string(vqlExpr));
      persistStore->repointCurrentVersion(parent);
      return true;
    } catch (const std::exception &err) {
      std::cerr << "VortexHost: failed to persist macro " << name << ": "
                << err.what() << "\n";
      return false;
    }
  }
  return true;
}

void VortexHost::loadMacrosFromStore(const xanadu::Store &keymapStore) {
  if (keymapStore.opCount() == 0 || keymapStore.homeCell() == noCell) {
    return;
  }
  const auto model = xanadu::SystemStoreModel::fromStore(keymapStore);
  for (const auto &s : model.settings()) {
    if (s.name.starts_with("macro.")) {
      std::string macroName     = s.name.substr(6);
      macroRegistry_[macroName] = s.value.asString(0);
    }
  }
}

xanadu::MicroversionId VortexHost::saveMacroToStore(
    std::string_view macroName, std::string_view vqlExpr,
    std::string_view keyBinding, xanadu::Store &keymapStore) {
  macroRegistry_[std::string(macroName)] = std::string(vqlExpr);
  auto parent                            = keymapStore.primaryCurrentVersion();
  if (keymapStore.homeCell() == noCell) {
    parent = xanadu::initializeSystemStoreGenesis(
        keymapStore, xanadu::SystemDocKind::Keymap, parent);
  } else {
    if (!hasKeymapGroups(keymapStore, parent)) {
      parent = xanadu::initializeSystemStoreGenesis(
          keymapStore, xanadu::SystemDocKind::Keymap, parent);
    }
  }
  xanadu::SettingSpec spec{
      .name    = "macro." + std::string(macroName),
      .notes   = "User macro: " + std::string(macroName),
      .schemas = {{.expectedTypes = {"string"},
                   .defaultValues = {std::string{vqlExpr}}}}};
  parent = xanadu::ensureSetting(keymapStore, parent, spec);
  parent =
      xanadu::setSetting(keymapStore, parent, "macro." + std::string(macroName),
                         std::string(vqlExpr));
  if (!keyBinding.empty()) {
    xanadu::SettingSpec bindSpec{
        .name    = std::string(macroName),
        .notes   = "Key binding for macro " + std::string(macroName),
        .schemas = {{.expectedTypes = {"string"},
                     .defaultValues = {std::string{keyBinding}}}}};
    parent = xanadu::ensureSetting(keymapStore, parent, bindSpec);
    parent = xanadu::setSetting(keymapStore, parent, macroName,
                                std::string(keyBinding));
  }
  keymapStore.repointCurrentVersion(parent);
  return parent;
}

std::optional<std::string> VortexHost::getMacro(std::string_view name) const {
  auto it = macroRegistry_.find(std::string(name));
  if (it != macroRegistry_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::vector<std::string> VortexHost::listMacros() const {
  std::vector<std::string> names;
  names.reserve(macroRegistry_.size());
  for (const auto &[name, _] : macroRegistry_) {
    names.push_back(name);
  }
  std::ranges::sort(names);
  return names;
}

} // namespace zigzag::vortex

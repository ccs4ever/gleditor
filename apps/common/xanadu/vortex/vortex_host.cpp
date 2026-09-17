/**
 * @file vortex_host.cpp
 * @brief Implementation of the VortexHost standardized public interface.
 */
#include "common/xanadu/vortex/vortex_host.hpp"

#include <algorithm>
#include <iostream>

#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace zigzag::vortex {

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

  return cfg;
}

VortexHost::VortexHost(const Manifold *baseManifold)
    : arena_(baseManifold), core_(arena_), vm_(core_), stdlib_(core_, vm_),
      vqlEngine_(core_), vqlCompiler_(core_, vm_) {
  initHostServices();
}

void VortexHost::initHostServices() {
  stdlib_.bootstrap();

  CellRef sweepOp = stdlib_.resolve("std:gc/sweep");
  if (sweepOp != noCell) {
    gcCursor_ = vm_.spawnCursor(sweepOp, "sys:gc");
  }
}

void VortexHost::bindManifold(const Manifold *baseManifold) {
  arena_ = ArenaManifold(baseManifold);
  initHostServices();
}

void VortexHost::bindStore(xanadu::Store *store) {
  boundStore_ = store;
  if (boundStore_) {
    loadConfigFromStore(*boundStore_);
  }
}

void VortexHost::loadConfigFromStore(const xanadu::Store &store) {
  config_ = VortexHostConfig::fromStore(store);
}

std::size_t VortexHost::stepScheduler(std::optional<std::size_t> cycleBudget) {
  const std::size_t budget = cycleBudget.value_or(config_.schedulerCycleBudget);
  std::size_t executed     = 0;
  for (std::size_t i = 0; i < budget; ++i) {
    std::size_t stepCount = vm_.stepScheduler();
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

std::size_t VortexHost::triggerGarbageCollection() { return stdlib_.gcSweep(); }

DimRef VortexHost::resolveDimRef(std::string_view dimName) {
  if (dimName == "d.spin") return core_.dims().spin;
  if (dimName == "d.grab") return core_.dims().grab;
  if (dimName == "d.step") return core_.dims().step;
  if (dimName == "d.contract") return core_.dims().contract;
  if (dimName == "d.clone") return core_.dims().clone;
  if (dimName == "d.cursors") return core_.dims().cursors;
  if (dimName == "d.cache") return core_.dims().cache;
  if (dimName == "d.vars") return core_.dims().vars;
  if (dimName == "d.values") return core_.dims().values;
  if (dimName == "d.pinning-cursors") return core_.dims().pinningCursors;
  if (dimName == "d.name") return core_.dims().name;
  if (dimName == "d.stdlib") return core_.dims().stdlib;
  if (dimName == "d.clause") return core_.dims().clause;

  return core_.mintDimension(dimName);
}

bool VortexHost::dispatchAction(std::string_view actionName, CellRef focusCell,
                                const ViewAxisBinding &axes,
                                CellRef &newFocusOut) {
  newFocusOut = focusCell;

  auto macroIt = macroRegistry_.find(std::string(actionName));
  if (macroIt != macroRegistry_.end()) {
    auto results = vqlEngine_.execute(macroIt->second);
    if (!results.empty()) {
      newFocusOut = results.back();
    }
    return true;
  }

  auto customIt = customActionRoutines_.find(std::string(actionName));
  if (customIt != customActionRoutines_.end()) {
    CellRef cursor = vm_.spawnCursor(customIt->second, actionName);
    auto res       = vm_.run(cursor, 1000);
    return res.success;
  }

  if (actionName == "step-x-pos") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.x_dimension),
                                 DimVector::POS);
    return true;
  }
  if (actionName == "step-x-neg") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.x_dimension),
                                 DimVector::NEG);
    return true;
  }
  if (actionName == "step-y-pos") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.y_dimension),
                                 DimVector::POS);
    return true;
  }
  if (actionName == "step-y-neg") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.y_dimension),
                                 DimVector::NEG);
    return true;
  }
  if (actionName == "step-z-pos") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.z_dimension),
                                 DimVector::POS);
    return true;
  }
  if (actionName == "step-z-neg") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.z_dimension),
                                 DimVector::NEG);
    return true;
  }

  if (actionName == "insert-cell-x-pos") {
    newFocusOut = stdlib_.zzInsert(focusCell, resolveDimRef(axes.x_dimension),
                                   DimVector::POS, "New Cell");
    return true;
  }
  if (actionName == "insert-cell-x-neg") {
    newFocusOut = stdlib_.zzInsert(focusCell, resolveDimRef(axes.x_dimension),
                                   DimVector::NEG, "New Cell");
    return true;
  }
  if (actionName == "insert-cell-y-pos") {
    newFocusOut = stdlib_.zzInsert(focusCell, resolveDimRef(axes.y_dimension),
                                   DimVector::POS, "New Cell");
    return true;
  }
  if (actionName == "insert-cell-y-neg") {
    newFocusOut = stdlib_.zzInsert(focusCell, resolveDimRef(axes.y_dimension),
                                   DimVector::NEG, "New Cell");
    return true;
  }

  if (actionName == "unlink-x-pos") {
    stdlib_.zzUnlink(focusCell, resolveDimRef(axes.x_dimension),
                     DimVector::POS);
    return true;
  }
  if (actionName == "unlink-x-neg") {
    stdlib_.zzUnlink(focusCell, resolveDimRef(axes.x_dimension),
                     DimVector::NEG);
    return true;
  }
  if (actionName == "delete-focus-cell") {
    stdlib_.zzDelete(focusCell);
    return true;
  }

  return false;
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
  return stdlib_.modules();
}

std::vector<std::string>
VortexHost::symbolsInModule(std::string_view modulePath) const {
  return stdlib_.symbolsInModule(modulePath);
}

CellRef VortexHost::cloneSymbolToChain(std::string_view symbolPath,
                                       CellRef targetCell) {
  if (symbolPath.starts_with("#")) {
    CellRef op = arena_.makeCell(symbolPath);
    if (targetCell != noCell && arena_.contains(targetCell)) {
      core_.link(targetCell, core_.dims().step, DimVector::POS, op);
    }
    return op;
  }
  CellRef sym = stdlib_.resolve(symbolPath);
  if (sym == noCell) {
    return noCell;
  }
  return stdlib_.zzCloneToChain(sym, targetCell);
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

  if (targetCell != noCell && arena_.contains(targetCell)) {
    const DimRef dim = resolveDimRef(attachDimension);
    core_.link(targetCell, dim, dir, result.entryOpcode);
  }

  if (spawnThread) {
    vm_.spawnCursor(result.entryOpcode, "vql_exec");
  }

  return result;
}

std::optional<zigzag::Promoted> VortexHost::promoteAndAttachToStore(
    CellRef entryOpcode, CellRef persistentTarget,
    std::string_view attachDimension, DimVector dir, xanadu::Store &store,
    const xanadu::MicroversionId &parent) {
  if (entryOpcode == noCell || !arena_.contains(entryOpcode)) {
    return std::nullopt;
  }

  auto promoted = zigzag::promote(store, parent, arena_, entryOpcode);
  if (!promoted || promoted->cells.empty()) {
    return std::nullopt;
  }

  if (persistentTarget != noCell && persistentTarget != zigzag::noCell) {
    auto manifold       = store.rebuildManifold(promoted->version);
    const DimRef dimRef = zigzag::DimensionRegistry::instance().getOrCreate(
        store, promoted->version, manifold, attachDimension);
    static_cast<void>(manifold.advance(store, promoted->version));
    const CellRef persistentEntryOp = promoted->cells.front();
    promoted->version =
        store.setLink(promoted->version, persistentTarget, dimRef, dir,
                      persistentEntryOp, &manifold);
  }

  return promoted;
}

std::vector<CellRef>
VortexHost::executeVQL(std::string_view queryOrWeave,
                       const std::vector<CellRef> &contextCells) {
  if (contextCells.empty()) {
    return vqlEngine_.execute(queryOrWeave);
  }
  return vqlEngine_.execute(queryOrWeave);
}

bool VortexHost::defineMacro(std::string_view name, std::string_view vqlExpr,
                             xanadu::Store *persistStore) {
  macroRegistry_[std::string(name)] = std::string(vqlExpr);

  if (persistStore) {
    try {
      auto parent = persistStore->primaryCurrentVersion();
      xanadu::setSetting(*persistStore, parent, "macro." + std::string(name),
                         std::string(vqlExpr));
      return true;
    } catch (const std::exception &err) {
      std::cerr << "VortexHost: failed to persist macro " << name << ": "
                << err.what() << "\n";
      return false;
    }
  }
  return true;
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
  std::sort(names.begin(), names.end());
  return names;
}

} // namespace zigzag::vortex

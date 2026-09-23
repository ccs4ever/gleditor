/**
 * @file vortex_host.cpp
 * @brief Implementation of the VortexHost standardized public interface.
 */
#include "common/xanadu/vortex/vortex_host.hpp"

#include <algorithm>
#include <format>
#include <iostream>

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

  return cfg;
}

VortexHost::VortexHost(const Manifold *baseManifold)
    : arena_(baseManifold), core_(arena_), vm_(core_), stdlib_(core_, vm_),
      vqlEngine_(core_), vqlCompiler_(core_, vm_), vplEngine_(core_) {
  initHostServices();
}

void VortexHost::initHostServices() {
  stdlib_.bootstrap();

  if (const auto sweepOp = stdlib_.resolve("std:gc/sweep")) {
    gcCursor_ = vm_.spawnCursor(*sweepOp, "sys:gc");
  }

  // Register native Vortex routines for keymap actions
  auto regPair = [&](std::string_view legacy, std::string_view canonical) {
    if (const auto op = stdlib_.resolve(canonical)) {
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
}

void VortexHost::bindManifold(const Manifold *baseManifold) {
  arena_ = ArenaManifold(baseManifold);
  initHostServices();
}

void VortexHost::bindStore(xanadu::Store *store) {
  boundStore_ = store;
  stdlib_.setBoundStore(store);
  vplEngine_.setStore(store);
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
    if (target.has_value() && *target != noCell) {
      newFocusOut = *target;
      return true;
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
  if (customIt != customActionRoutines_.end() && customIt->second != noCell) {
    CellRef cursor = vm_.spawnCursor(customIt->second, actionName);
    static_cast<void>(vm_.run(cursor, 1000));
  }

  // Application action delegate hook (e.g. apps/xudu, apps/zigzag)
  if (appActionDelegate_ &&
      appActionDelegate_(canonical, focusCell, axes, newFocusOut)) {
    return true;
  }

  // Navigation actions
  if (actionName == "step-x-pos" || canonical == "std:nav/step_x_pos") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.x_dimension),
                                 DimVector::POS);
    return true;
  }
  if (actionName == "step-x-neg" || canonical == "std:nav/step_x_neg") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.x_dimension),
                                 DimVector::NEG);
    return true;
  }
  if (actionName == "step-y-pos" || canonical == "std:nav/step_y_pos") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.y_dimension),
                                 DimVector::POS);
    return true;
  }
  if (actionName == "step-y-neg" || canonical == "std:nav/step_y_neg") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.y_dimension),
                                 DimVector::NEG);
    return true;
  }
  if (actionName == "step-z-pos" || canonical == "std:nav/step_z_pos") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.z_dimension),
                                 DimVector::POS);
    return true;
  }
  if (actionName == "step-z-neg" || canonical == "std:nav/step_z_neg") {
    newFocusOut = stdlib_.zzStep(focusCell, resolveDimRef(axes.z_dimension),
                                 DimVector::NEG);
    return true;
  }
  if (actionName == "hop-head" || canonical == "std:nav/hop_head") {
    newFocusOut = stdlib_.hopHead(focusCell, resolveDimRef(axes.x_dimension));
    return true;
  }
  if (actionName == "hop-tail" || canonical == "std:nav/hop_tail") {
    newFocusOut = stdlib_.hopTail(focusCell, resolveDimRef(axes.x_dimension));
    return true;
  }
  if (actionName == "jump-home" || canonical == "std:nav/jump_home") {
    newFocusOut = stdlib_.jumpHome();
    return true;
  }

  // Editing actions
  if (actionName == "insert-cell-x-pos" ||
      canonical == "std:zigzag/insert_cell_x_pos") {
    newFocusOut = stdlib_.zzInsert(focusCell, resolveDimRef(axes.x_dimension),
                                   DimVector::POS, "New Cell");
    return true;
  }
  if (actionName == "insert-cell-x-neg" ||
      canonical == "std:zigzag/insert_cell_x_neg") {
    newFocusOut = stdlib_.zzInsert(focusCell, resolveDimRef(axes.x_dimension),
                                   DimVector::NEG, "New Cell");
    return true;
  }
  if (actionName == "insert-cell-y-pos" ||
      canonical == "std:zigzag/insert_cell_y_pos") {
    newFocusOut = stdlib_.zzInsert(focusCell, resolveDimRef(axes.y_dimension),
                                   DimVector::POS, "New Cell");
    return true;
  }
  if (actionName == "insert-cell-y-neg" ||
      canonical == "std:zigzag/insert_cell_y_neg") {
    newFocusOut = stdlib_.zzInsert(focusCell, resolveDimRef(axes.y_dimension),
                                   DimVector::NEG, "New Cell");
    return true;
  }
  if (actionName == "unlink-x-pos" || canonical == "std:zigzag/unlink_x_pos") {
    stdlib_.zzUnlink(focusCell, resolveDimRef(axes.x_dimension),
                     DimVector::POS);
    return true;
  }
  if (actionName == "unlink-x-neg" || canonical == "std:zigzag/unlink_x_neg") {
    stdlib_.zzUnlink(focusCell, resolveDimRef(axes.x_dimension),
                     DimVector::NEG);
    return true;
  }
  if (actionName == "delete-focus-cell" ||
      canonical == "std:zigzag/delete_focus_cell" ||
      canonical == "std:zigzag/delete_focus_cell_bksp") {
    stdlib_.zzDelete(focusCell);
    return true;
  }
  if (actionName == "duplicate-focus-cell" ||
      canonical == "std:zigzag/duplicate") {
    newFocusOut = stdlib_.zzDuplicate(focusCell);
    if (boundStore_ && newFocusOut != noCell &&
        zigzag::isEphemeral(newFocusOut)) {
      zigzag::PromotionBudget budget{.maxOps = 1000};
      auto promoted =
          zigzag::promote(*boundStore_, boundStore_->primaryCurrentVersion(),
                          arena_, newFocusOut, budget);
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
    stdlib_.swapAxes(axes);
    return true;
  }
  if (actionName == "cycle-dims-forward" ||
      canonical == "std:ui/cycle_dims_forward") {
    stdlib_.cycleDims(axes, true);
    return true;
  }
  if (actionName == "cycle-dims-backward" ||
      canonical == "std:ui/cycle_dims_backward") {
    stdlib_.cycleDims(axes, false);
    return true;
  }
  if (actionName == "bundle-execution" ||
      canonical == "std:ui/bundle_execution") {
    stdlib_.applyBundle(axes, DimensionBundle::Execution);
    return true;
  }
  if (actionName == "bundle-scope" || canonical == "std:ui/bundle_scope") {
    stdlib_.applyBundle(axes, DimensionBundle::Scope);
    return true;
  }
  if (actionName == "bundle-contract" ||
      canonical == "std:ui/bundle_contract") {
    stdlib_.applyBundle(axes, DimensionBundle::Contract);
    return true;
  }
  if (actionName == "bundle-logic" || canonical == "std:ui/bundle_logic") {
    stdlib_.applyBundle(axes, DimensionBundle::Logic);
    return true;
  }
  if (actionName == "bundle-stdlib" || canonical == "std:ui/bundle_stdlib") {
    stdlib_.applyBundle(axes, DimensionBundle::Stdlib);
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
    stdlib_.applyBundle(axes, next);
    return true;
  }

  // Stdlib fallback: check if standard library has a registered routine cell
  const auto stdlibOp = stdlib_.resolve(canonical).or_else([&] {
    return canonical != actionName ? stdlib_.resolve(actionName) : std::nullopt;
  });
  if (stdlibOp) {
    CellRef cursor = vm_.spawnCursor(*stdlibOp, canonical);
    static_cast<void>(vm_.run(cursor, 1000));
    return true;
  }

  return false;
}

void VortexHost::setView(ViewAxisBinding &axes, std::string_view dimX,
                         std::string_view dimY, std::string_view dimZ) {
  stdlib_.setView(axes, dimX, dimY, dimZ);
}

bool VortexHost::exportLibrary(std::string_view moduleName,
                               const std::string &destinationPath) const {
  xanadu::Store store;
  if (!stdlib_.exportModuleToStore(moduleName, store)) {
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
  if (!stdlib_.exportStandardLibraryToStore(store)) {
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
  CellRef res = stdlib_.importModuleFromStore(store);
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
  return stdlib_.resolve(symbolPath)
      .transform([&](const CellRef sym) {
        return stdlib_.zzCloneToChain(sym, targetCell);
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
            static_cast<void>(zigzag::promote(
                *store, store->primaryCurrentVersion(), arena_, c));
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
          static_cast<void>(zigzag::promote(
              *store, store->primaryCurrentVersion(), arena_, c));
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
    res.affectedCells = view.collectCells(arena_);
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
      auto sh              = view.shape(arena_);
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
    xanadu::vprolog::Compiler compiler(core_);
    auto query = compiler.compileQuery(goalQuery);
    if (query.goals.empty()) {
      return {};
    }
    return stdlib_.solveQuery(query.goals, compiler.customPredicates(),
                              maxSolutions);
  } catch (const std::exception &err) {
    std::cerr << "VortexHost::solveLogic error: " << err.what() << "\n";
    return {};
  }
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

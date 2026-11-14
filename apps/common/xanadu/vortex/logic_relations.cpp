/**
 * @file logic_relations.cpp
 * @brief Card P3: the RelationNext machinery (retry-loop emitter, VM provider
 * adapter) and the stdlib-registry and store providers.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex::logic {

// Abstract base class for providers that bind to VortexCore at install time.
class CoreBoundProvider : public RelationProvider {
public:
  void bindCore(VortexCore *c) noexcept { core_ = c; }

protected:
  VortexCore *core_{nullptr};
};

namespace {

inline constexpr std::string_view kAllRelationNames[] = {
    "vortex_module",      "stdlib_module",
    "vortex_function",    "stdlib_function",
    "vortex_instruction", "vortex_contract",
    "vortex_param",       "setting",
    "setting_shape",      "setting_default",
    "transclude",         "xanalink",
    "cell_value",         "cell_link",
    "cell_span",          "bridge_edge",
};

// =============================================================================
// Helper functions for introspection & settings
// =============================================================================

struct FunctionItem {
  std::string mod;
  std::string sym;
};

std::vector<FunctionItem> allFunctions(VortexStdLib &stdlib) {
  std::vector<FunctionItem> items;
  for (const auto &m : stdlib.modules()) {
    for (const auto &s : stdlib.symbolsInModule(m)) {
      items.push_back({m, s});
    }
  }
  return items;
}

struct InstructionItem {
  std::string path;
  std::int64_t step{};
  std::string label;
};

std::vector<InstructionItem>
collectInstructions(VortexCore &core, VortexStdLib &stdlib,
                    const std::optional<std::string> &boundPath) {
  std::vector<InstructionItem> items;
  const auto collectForPath = [&](const std::string &fullPath) {
    const auto resolved = stdlib.resolve(fullPath);
    if (!resolved) return;
    std::optional<CellRef> curOp = *resolved;
    std::int64_t stepIdx         = 0;
    std::size_t limit            = core.arena().cellCount() + 1;
    while (curOp && limit-- > 0) {
      core.arena().consumeWork();
      items.push_back({fullPath, stepIdx++, core.arena().textOf(*curOp)});
      curOp = core.arena().linked(*curOp, core.dims().spin, DimVector::POS);
    }
  };

  if (boundPath) {
    collectForPath(*boundPath);
  } else {
    for (const auto &m : stdlib.modules()) {
      core.arena().consumeWork();
      for (const auto &s : stdlib.symbolsInModule(m)) {
        core.arena().consumeWork();
        collectForPath(m + "/" + s);
      }
    }
  }
  return items;
}

struct ContractItem {
  std::string path;
  std::string type;
  std::string label;
};

std::vector<ContractItem>
collectContracts(VortexCore &core, VortexStdLib &stdlib,
                 const std::optional<std::string> &boundPath) {
  std::vector<ContractItem> items;
  const auto collectForPath = [&](const std::string &fullPath) {
    const auto resolved = stdlib.resolve(fullPath);
    if (!resolved) return;
    std::optional<CellRef> curOp = *resolved;
    std::size_t limit            = core.arena().cellCount() + 1;
    while (curOp && limit-- > 0) {
      core.arena().consumeWork();
      for (const CellRef p : core.preconditionsOf(*curOp)) {
        core.arena().consumeWork();
        items.push_back({fullPath, "precondition", core.arena().textOf(p)});
      }
      for (const CellRef p : core.postconditionsOf(*curOp)) {
        core.arena().consumeWork();
        items.push_back({fullPath, "postcondition", core.arena().textOf(p)});
      }
      curOp = core.arena().linked(*curOp, core.dims().spin, DimVector::POS);
    }
  };

  if (boundPath) {
    collectForPath(*boundPath);
  } else {
    for (const auto &m : stdlib.modules()) {
      core.arena().consumeWork();
      for (const auto &s : stdlib.symbolsInModule(m)) {
        core.arena().consumeWork();
        collectForPath(m + "/" + s);
      }
    }
  }
  return items;
}

struct ParamItem {
  std::string path;
  std::string wing;
  std::int64_t slotIndex{};
  CellRef target{};
};

std::vector<ParamItem>
collectParams(VortexCore &core, VortexStdLib &stdlib,
              const std::optional<std::string> &boundPath) {
  std::vector<ParamItem> items;
  const auto collectForPath = [&](const std::string &fullPath) {
    const auto resolved = stdlib.resolve(fullPath);
    if (!resolved) return;
    const CellRef entryOp = *resolved;
    const auto inCells    = core.inputsOf(entryOp);
    for (std::size_t i = 0; i < inCells.size(); ++i) {
      core.arena().consumeWork();
      items.push_back(
          {fullPath, "input", static_cast<std::int64_t>(i), inCells[i]});
    }
    const auto outCells = core.outputsOf(entryOp);
    for (std::size_t i = 0; i < outCells.size(); ++i) {
      core.arena().consumeWork();
      items.push_back(
          {fullPath, "output", static_cast<std::int64_t>(i), outCells[i]});
    }
  };

  if (boundPath) {
    collectForPath(*boundPath);
  } else {
    for (const auto &m : stdlib.modules()) {
      core.arena().consumeWork();
      for (const auto &s : stdlib.symbolsInModule(m)) {
        core.arena().consumeWork();
        collectForPath(m + "/" + s);
      }
    }
  }
  return items;
}

CellRef makeSettingValueCell(VortexCore &core, VortexStdLib &stdlib,
                             const xanadu::SettingValue &val) {
  if (val.elements.empty()) {
    return core.arena().makeCell("");
  }
  if (val.elements.size() == 1) {
    const auto &el = val.elements[0];
    if (std::holds_alternative<double>(el)) {
      return core.arena().makeScalarCell(std::get<double>(el));
    }
    if (std::holds_alternative<std::int64_t>(el)) {
      return core.arena().makeScalarCell(std::get<std::int64_t>(el));
    }
    if (std::holds_alternative<bool>(el)) {
      return core.arena().makeScalarCell(std::get<bool>(el));
    }
    return core.arena().makeCell(std::get<std::string>(el));
  }
  std::vector<CellRef> elCells;
  elCells.reserve(val.elements.size());
  for (const auto &el : val.elements) {
    core.arena().consumeWork();
    if (std::holds_alternative<double>(el)) {
      elCells.push_back(core.arena().makeScalarCell(std::get<double>(el)));
    } else if (std::holds_alternative<std::int64_t>(el)) {
      elCells.push_back(
          core.arena().makeScalarCell(std::get<std::int64_t>(el)));
    } else if (std::holds_alternative<bool>(el)) {
      elCells.push_back(core.arena().makeScalarCell(std::get<bool>(el)));
    } else {
      elCells.push_back(core.arena().makeCell(std::get<std::string>(el)));
    }
  }
  return stdlib.makeList(elCells);
}

CellRef
makeSettingDefaultCell(VortexCore &core, VortexStdLib &stdlib,
                       const std::vector<xanadu::CellValue> &defaultValues) {
  if (defaultValues.empty()) {
    return core.arena().makeCell("");
  }
  if (defaultValues.size() == 1) {
    const auto &el = defaultValues[0];
    if (std::holds_alternative<double>(el)) {
      return core.arena().makeScalarCell(std::get<double>(el));
    }
    if (std::holds_alternative<std::int64_t>(el)) {
      return core.arena().makeScalarCell(std::get<std::int64_t>(el));
    }
    if (std::holds_alternative<bool>(el)) {
      return core.arena().makeScalarCell(std::get<bool>(el));
    }
    return core.arena().makeCell(std::get<std::string>(el));
  }
  std::vector<CellRef> defCells;
  defCells.reserve(defaultValues.size());
  for (const auto &el : defaultValues) {
    core.arena().consumeWork();
    if (std::holds_alternative<double>(el)) {
      defCells.push_back(core.arena().makeScalarCell(std::get<double>(el)));
    } else if (std::holds_alternative<std::int64_t>(el)) {
      defCells.push_back(
          core.arena().makeScalarCell(std::get<std::int64_t>(el)));
    } else if (std::holds_alternative<bool>(el)) {
      defCells.push_back(core.arena().makeScalarCell(std::get<bool>(el)));
    } else {
      defCells.push_back(core.arena().makeCell(std::get<std::string>(el)));
    }
  }
  return stdlib.makeList(defCells);
}

// =============================================================================
// Relation Providers
// =============================================================================

class VortexModuleProvider : public CoreBoundProvider {
public:
  explicit VortexModuleProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 1; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;
    const auto mods   = stdlib_.modules();
    const auto limit  = request.limit.value_or(mods.size());
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);

    for (std::size_t i = request.position; i < limit && i < mods.size(); ++i) {
      core_->arena().consumeWork();
      const auto &modName = mods[i];
      if (!isVar0 && core_->arena().textOf(*bound0) != modName) continue;
      const CellRef mCell = core_->arena().makeCell(modName);
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {mCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class VortexFunctionProvider1 : public CoreBoundProvider {
public:
  explicit VortexFunctionProvider1(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 1; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;
    const auto items  = allFunctions(stdlib_);
    const auto limit  = request.limit.value_or(items.size());
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);

    for (std::size_t i = request.position; i < limit && i < items.size(); ++i) {
      core_->arena().consumeWork();
      const auto &item = items[i];
      if (!isVar0 && core_->arena().textOf(*bound0) != item.sym) continue;
      const CellRef fCell = core_->arena().makeCell(item.sym);
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {fCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class VortexFunctionProvider2 : public CoreBoundProvider {
public:
  explicit VortexFunctionProvider2(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 2; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;
    const auto items  = allFunctions(stdlib_);
    const auto limit  = request.limit.value_or(items.size());
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);
    const auto bound1 = stdlib_.deref(request.arguments[1]);
    const bool isVar1 = !bound1 || stdlib_.isVar(*bound1);

    for (std::size_t i = request.position; i < limit && i < items.size(); ++i) {
      core_->arena().consumeWork();
      const auto &item = items[i];
      if (!isVar0 && core_->arena().textOf(*bound0) != item.mod) continue;
      if (!isVar1 && core_->arena().textOf(*bound1) != item.sym) continue;
      const CellRef mCell = core_->arena().makeCell(item.mod);
      const CellRef fCell = core_->arena().makeCell(item.sym);
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {mCell, fCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class VortexFunctionProvider3 : public CoreBoundProvider {
public:
  explicit VortexFunctionProvider3(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 3; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;
    const auto items  = allFunctions(stdlib_);
    const auto limit  = request.limit.value_or(items.size());
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);
    const auto bound1 = stdlib_.deref(request.arguments[1]);
    const bool isVar1 = !bound1 || stdlib_.isVar(*bound1);
    const auto bound2 = stdlib_.deref(request.arguments[2]);
    const bool isVar2 = !bound2 || stdlib_.isVar(*bound2);

    for (std::size_t i = request.position; i < limit && i < items.size(); ++i) {
      core_->arena().consumeWork();
      const auto &item       = items[i];
      const std::string path = item.mod + "/" + item.sym;
      if (!isVar0 && core_->arena().textOf(*bound0) != item.mod) continue;
      if (!isVar1 && core_->arena().textOf(*bound1) != item.sym) continue;
      if (!isVar2 && core_->arena().textOf(*bound2) != path) continue;
      const CellRef mCell = core_->arena().makeCell(item.mod);
      const CellRef fCell = core_->arena().makeCell(item.sym);
      const CellRef pCell = core_->arena().makeCell(path);
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {mCell, fCell, pCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class VortexInstructionProvider : public CoreBoundProvider {
public:
  explicit VortexInstructionProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 3; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);
    const std::optional<std::string> pathFilter =
        isVar0 ? std::nullopt
               : std::optional<std::string>{core_->arena().textOf(*bound0)};

    const auto items = collectInstructions(*core_, stdlib_, pathFilter);
    const auto limit = request.limit.value_or(items.size());

    const auto bound1 = stdlib_.deref(request.arguments[1]);
    const bool isVar1 = !bound1 || stdlib_.isVar(*bound1);
    const auto bound2 = stdlib_.deref(request.arguments[2]);
    const bool isVar2 = !bound2 || stdlib_.isVar(*bound2);

    for (std::size_t i = request.position; i < limit && i < items.size(); ++i) {
      core_->arena().consumeWork();
      const auto &item = items[i];
      if (!isVar1) {
        const auto stepVal = core_->arena().asInt64(*bound1);
        if (!stepVal || *stepVal != item.step) continue;
      }
      if (!isVar2 && core_->arena().textOf(*bound2) != item.label) continue;

      const CellRef pCell   = core_->arena().makeCell(item.path);
      const CellRef idxCell = core_->arena().makeScalarCell(item.step);
      const CellRef lblCell = core_->arena().makeCell(item.label);
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {pCell, idxCell, lblCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class VortexContractProvider : public CoreBoundProvider {
public:
  explicit VortexContractProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 3; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);
    const std::optional<std::string> pathFilter =
        isVar0 ? std::nullopt
               : std::optional<std::string>{core_->arena().textOf(*bound0)};

    const auto items = collectContracts(*core_, stdlib_, pathFilter);
    const auto limit = request.limit.value_or(items.size());

    const auto bound1 = stdlib_.deref(request.arguments[1]);
    const bool isVar1 = !bound1 || stdlib_.isVar(*bound1);
    const auto bound2 = stdlib_.deref(request.arguments[2]);
    const bool isVar2 = !bound2 || stdlib_.isVar(*bound2);

    for (std::size_t i = request.position; i < limit && i < items.size(); ++i) {
      core_->arena().consumeWork();
      const auto &item = items[i];
      if (!isVar1 && core_->arena().textOf(*bound1) != item.type) continue;
      if (!isVar2 && core_->arena().textOf(*bound2) != item.label) continue;

      const CellRef pCell    = core_->arena().makeCell(item.path);
      const CellRef typeCell = core_->arena().makeCell(item.type);
      const CellRef lblCell  = core_->arena().makeCell(item.label);
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {pCell, typeCell, lblCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class VortexParamProvider : public CoreBoundProvider {
public:
  explicit VortexParamProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 4; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);
    const std::optional<std::string> pathFilter =
        isVar0 ? std::nullopt
               : std::optional<std::string>{core_->arena().textOf(*bound0)};

    const auto items = collectParams(*core_, stdlib_, pathFilter);
    const auto limit = request.limit.value_or(items.size());

    const auto bound1 = stdlib_.deref(request.arguments[1]);
    const bool isVar1 = !bound1 || stdlib_.isVar(*bound1);
    const auto bound2 = stdlib_.deref(request.arguments[2]);
    const bool isVar2 = !bound2 || stdlib_.isVar(*bound2);
    const auto bound3 = stdlib_.deref(request.arguments[3]);
    const bool isVar3 = !bound3 || stdlib_.isVar(*bound3);

    for (std::size_t i = request.position; i < limit && i < items.size(); ++i) {
      core_->arena().consumeWork();
      const auto &item = items[i];
      if (!isVar1 && core_->arena().textOf(*bound1) != item.wing) continue;
      if (!isVar2) {
        const auto slotVal = core_->arena().asInt64(*bound2);
        if (!slotVal || *slotVal != item.slotIndex) continue;
      }
      if (!isVar3) {
        const auto targetVal = core_->arena().asInt64(*bound3);
        if (!targetVal || *targetVal != static_cast<std::int64_t>(item.target))
          continue;
      }

      const CellRef pCell   = core_->arena().makeCell(item.path);
      const CellRef wCell   = core_->arena().makeCell(item.wing);
      const CellRef idxCell = core_->arena().makeScalarCell(item.slotIndex);
      const CellRef slotCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(item.target));
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {pCell, wCell, idxCell, slotCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class SettingProvider : public CoreBoundProvider {
public:
  explicit SettingProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 2; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);

    if (stdlib_.boundStore() != nullptr) {
      const auto model =
          xanadu::SystemStoreModel::fromStore(*stdlib_.boundStore());
      const auto &settings = model.settings();
      const auto limit     = request.limit.value_or(settings.size());

      for (std::size_t i = request.position; i < limit && i < settings.size();
           ++i) {
        core_->arena().consumeWork();
        const auto &s = settings[i];
        if (!isVar0 && core_->arena().textOf(*bound0) != s.name) continue;

        const CellRef nameCell = core_->arena().makeCell(s.name);
        const CellRef valCell  = makeSettingValueCell(*core_, stdlib_, s.value);
        return RelationAnswer{
            .nextPosition = i + 1,
            .limit        = limit,
            .cells        = {nameCell, valCell},
        };
      }
    } else {
      // Unbound store: walk home() along d.vars
      const DimRef varsDim = core_->dims().vars;
      const DimRef valsDim = core_->dims().values;
      std::vector<std::pair<std::string, CellRef>> entries;
      std::optional<CellRef> curVar =
          core_->arena().linked(core_->home(), varsDim, DimVector::POS);
      std::size_t countLimit = core_->arena().cellCount() + 1;
      while (curVar && countLimit-- > 0) {
        core_->arena().consumeWork();
        const std::string sName = core_->arena().textOf(*curVar);
        const auto curVal =
            core_->arena().linked(*curVar, valsDim, DimVector::POS);
        entries.emplace_back(sName,
                             curVal.value_or(core_->arena().makeCell("")));
        curVar = core_->arena().linked(*curVar, varsDim, DimVector::POS);
      }
      const auto limit = request.limit.value_or(entries.size());
      for (std::size_t i = request.position; i < limit && i < entries.size();
           ++i) {
        core_->arena().consumeWork();
        const auto &entry = entries[i];
        if (!isVar0 && core_->arena().textOf(*bound0) != entry.first) continue;

        const CellRef nameCell = core_->arena().makeCell(entry.first);
        return RelationAnswer{
            .nextPosition = i + 1,
            .limit        = limit,
            .cells        = {nameCell, entry.second},
        };
      }
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class SettingShapeProvider : public CoreBoundProvider {
public:
  explicit SettingShapeProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 2; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_ || stdlib_.boundStore() == nullptr) return std::nullopt;

    struct ShapeItem {
      std::string name;
      std::vector<std::string> types;
    };
    const auto model =
        xanadu::SystemStoreModel::fromStore(*stdlib_.boundStore());
    std::vector<ShapeItem> items;
    for (const auto &s : model.settings()) {
      core_->arena().consumeWork();
      for (const auto &alt : s.schema.alternatives) {
        core_->arena().consumeWork();
        items.push_back({s.name, alt.expectedTypes});
      }
    }

    const auto limit  = request.limit.value_or(items.size());
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);

    for (std::size_t i = request.position; i < limit && i < items.size(); ++i) {
      core_->arena().consumeWork();
      const auto &item = items[i];
      if (!isVar0 && core_->arena().textOf(*bound0) != item.name) continue;

      const CellRef nameCell = core_->arena().makeCell(item.name);
      std::vector<CellRef> typeCells;
      typeCells.reserve(item.types.size());
      for (const auto &t : item.types) {
        core_->arena().consumeWork();
        typeCells.push_back(core_->arena().makeCell(t));
      }
      const CellRef shapeCell = stdlib_.makeList(typeCells);
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {nameCell, shapeCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class SettingDefaultProvider : public CoreBoundProvider {
public:
  explicit SettingDefaultProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 2; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_ || stdlib_.boundStore() == nullptr) return std::nullopt;

    struct DefaultItem {
      std::string name;
      std::vector<xanadu::CellValue> defaults;
    };
    const auto model =
        xanadu::SystemStoreModel::fromStore(*stdlib_.boundStore());
    std::vector<DefaultItem> items;
    for (const auto &s : model.settings()) {
      core_->arena().consumeWork();
      for (const auto &alt : s.schema.alternatives) {
        core_->arena().consumeWork();
        items.push_back({s.name, alt.defaultValues});
      }
    }

    const auto limit  = request.limit.value_or(items.size());
    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);

    for (std::size_t i = request.position; i < limit && i < items.size(); ++i) {
      core_->arena().consumeWork();
      const auto &item = items[i];
      if (!isVar0 && core_->arena().textOf(*bound0) != item.name) continue;

      const CellRef nameCell = core_->arena().makeCell(item.name);
      const CellRef defCell =
          makeSettingDefaultCell(*core_, stdlib_, item.defaults);
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {nameCell, defCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class TranscludeProvider : public CoreBoundProvider {
public:
  explicit TranscludeProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 3; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_ || stdlib_.boundStore() == nullptr) return std::nullopt;

    const auto &store = *stdlib_.boundStore();
    const auto limit  = request.limit.value_or(store.opCount());

    for (std::size_t i = request.position; i < limit && i < store.opCount();
         ++i) {
      core_->arena().consumeWork();
      const auto *cop = store.getCompactOp(static_cast<std::uint32_t>(i));
      if (cop != nullptr && cop->kind == xanadu::OpKind::Transclude) {
        const CellRef oDocCell    = core_->arena().makeCell("store");
        const std::string spanStr = "[" + std::to_string(cop->spanStart) + "," +
                                    std::to_string(cop->spanLength) + "]";
        const CellRef spanCell = core_->arena().makeCell(spanStr);
        const CellRef tDocCell = core_->arena().makeCell("active");
        return RelationAnswer{
            .nextPosition = i + 1,
            .limit        = limit,
            .cells        = {oDocCell, spanCell, tDocCell},
        };
      }
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

class XanalinkProvider : public CoreBoundProvider {
public:
  explicit XanalinkProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 3; }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_ || stdlib_.boundStore() == nullptr) return std::nullopt;

    const auto &store = *stdlib_.boundStore();
    std::vector<xanadu::Link> links;
    for (const auto &link : store.linkView()) {
      links.push_back(link);
    }
    const auto limit = request.limit.value_or(links.size());

    for (std::size_t i = request.position; i < limit && i < links.size(); ++i) {
      core_->arena().consumeWork();
      const auto &link = links[i];
      const std::string leftStr =
          link.left.empty() ? "[]"
                            : ("[" + std::to_string(link.left[0].start) + "," +
                               std::to_string(link.left[0].length) + "]");
      const std::string rightStr =
          link.right.empty()
              ? "[]"
              : ("[" + std::to_string(link.right[0].start) + "," +
                 std::to_string(link.right[0].length) + "]");
      const std::string typeStr =
          link.owner.empty() ? std::string(xanadu::linkTypeName(link.type))
                             : link.owner;

      const CellRef lCell = core_->arena().makeCell(leftStr);
      const CellRef tCell = core_->arena().makeCell(typeStr);
      const CellRef rCell = core_->arena().makeCell(rightStr);
      return RelationAnswer{
          .nextPosition = i + 1,
          .limit        = limit,
          .cells        = {lCell, tCell, rCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

} // namespace

// =============================================================================
// Registration & Loop Emission
// =============================================================================

void registerStdlibRelations(RelationRegistry &registry, VortexStdLib &stdlib) {
  registry.add("vortex_module", std::make_unique<VortexModuleProvider>(stdlib));
  registry.add("stdlib_module", std::make_unique<VortexModuleProvider>(stdlib));

  registry.add("vortex_function",
               std::make_unique<VortexFunctionProvider1>(stdlib));
  registry.add("vortex_function",
               std::make_unique<VortexFunctionProvider2>(stdlib));
  registry.add("vortex_function",
               std::make_unique<VortexFunctionProvider3>(stdlib));
  registry.add("stdlib_function",
               std::make_unique<VortexFunctionProvider1>(stdlib));
  registry.add("stdlib_function",
               std::make_unique<VortexFunctionProvider2>(stdlib));
  registry.add("stdlib_function",
               std::make_unique<VortexFunctionProvider3>(stdlib));

  registry.add("vortex_instruction",
               std::make_unique<VortexInstructionProvider>(stdlib));
  registry.add("vortex_contract",
               std::make_unique<VortexContractProvider>(stdlib));
  registry.add("vortex_param", std::make_unique<VortexParamProvider>(stdlib));

  registry.add("setting", std::make_unique<SettingProvider>(stdlib));
  registry.add("setting_shape", std::make_unique<SettingShapeProvider>(stdlib));
  registry.add("setting_default",
               std::make_unique<SettingDefaultProvider>(stdlib));

  registry.add("transclude", std::make_unique<TranscludeProvider>(stdlib));
  registry.add("xanalink", std::make_unique<XanalinkProvider>(stdlib));
}

void emitRelationGoal(CompileContext &ctx, FrameScope &scope, Chain &chain,
                      const PredicateKey &key, const CellRef goal) {
  const auto rawArgs = ctx.stdlib.argumentsOf(goal);
  if (rawArgs.size() != key.arity) {
    throw LogicRefusal("relation " + key.display() + " arity mismatch");
  }

  // 1. Term slots for each argument of the goal.
  std::vector<CellRef> argSlots;
  argSlots.reserve(key.arity);
  for (const CellRef rawArg : rawArgs) {
    argSlots.push_back(emitTerm(ctx, scope, chain, rawArg));
  }

  // 2. Static constants for relation key and 0.
  const CellRef keyCell  = emitConstant(ctx, chain, key.name);
  const CellRef zeroCell = emitConstant(ctx, chain, std::int64_t{0});

  // 3. Locals for position and limit, initialized to 0.
  const CellRef pos   = newLocal(ctx, scope);
  const CellRef limit = newLocal(ctx, scope);
  const std::array<CellRef, 2> zeroInputs{zeroCell, zeroCell};
  const std::array<CellRef, 1> posOutputs{pos};
  const std::array<CellRef, 1> limitOutputs{limit};
  appendOp(ctx, chain, OpcodeKind::Add, zeroInputs, posOutputs);
  appendOp(ctx, chain, OpcodeKind::Add, zeroInputs, limitOutputs);

  // 4. Loop head label.
  const Label loopLabel = newLabel(ctx);
  placeLabel(ctx, chain, loopLabel);

  // 5. Output slots for RelationNext.
  const CellRef foundSlot     = newLocal(ctx, scope);
  const CellRef nextPosSlot   = newLocal(ctx, scope);
  const CellRef nextLimitSlot = newLocal(ctx, scope);
  std::vector<CellRef> ansSlots;
  ansSlots.reserve(key.arity);
  for (std::size_t i = 0; i < key.arity; ++i) {
    ansSlots.push_back(newLocal(ctx, scope));
  }

  // 6. Append RelationNext opcode.
  std::vector<CellRef> inputs{keyCell, pos, limit};
  inputs.insert(inputs.end(), argSlots.begin(), argSlots.end());

  std::vector<CellRef> outputs{foundSlot, nextPosSlot, nextLimitSlot};
  outputs.insert(outputs.end(), ansSlots.begin(), ansSlots.end());

  appendOp(ctx, chain, OpcodeKind::RelationNext, inputs, outputs);

  // 7. If not found, fail (exhausted).
  emitFailUnless(ctx, scope, chain, foundSlot);

  // 8. Update pos and limit with nextPos and nextLimit.
  const std::array<CellRef, 2> nextPosInputs{nextPosSlot, zeroCell};
  appendOp(ctx, chain, OpcodeKind::Add, nextPosInputs, posOutputs);

  const std::array<CellRef, 2> nextLimitInputs{nextLimitSlot, zeroCell};
  appendOp(ctx, chain, OpcodeKind::Add, nextLimitInputs, limitOutputs);

  // 9. Choice point to retry at loopLabel.
  emitChoice(ctx, chain, loopLabel);

  // 10. Unify each argument slot with its answer slot.
  for (std::size_t i = 0; i < key.arity; ++i) {
    emitUnifyOrFail(ctx, scope, chain, argSlots[i], ansSlots[i]);
  }
}

void installRelationProvider(CompileContext &ctx) {
  for (const auto nameView : kAllRelationNames) {
    const std::string name(nameView);
    bool hasAny = false;
    for (std::size_t arity = 1; arity <= 4; ++arity) {
      if (auto *provider = ctx.relations.find(PredicateKey{name, arity})) {
        hasAny = true;
        if (auto *bound = dynamic_cast<CoreBoundProvider *>(provider)) {
          bound->bindCore(&ctx.core);
        }
      }
    }
    if (!hasAny) continue;

    ctx.vm.setRelationProvider(
        name,
        [&ctx](const zigzag::vortex::RelationQuery &query)
            -> zigzag::vortex::RelationResult {
          const PredicateKey key{std::string(query.relation),
                                 query.arguments.size()};
          auto *provider = ctx.relations.find(key);
          if (!provider) {
            return std::unexpected("RelationNext: no provider for " +
                                   key.display());
          }
          const RelationRequest req{
              .arguments = query.arguments,
              .position  = static_cast<std::size_t>(
                  std::max<std::int64_t>(0, query.position)),
              .limit = (query.position == 0 && query.limit == 0)
                           ? std::nullopt
                           : std::optional<std::size_t>(
                                 static_cast<std::size_t>(query.limit)),
          };
          const auto ans = provider->next(req);
          if (!ans) {
            return std::optional<zigzag::vortex::RelationAnswer>(std::nullopt);
          }
          return std::optional<zigzag::vortex::RelationAnswer>(
              zigzag::vortex::RelationAnswer{
                  .nextPosition = static_cast<std::int64_t>(ans->nextPosition),
                  .limit        = static_cast<std::int64_t>(ans->limit),
                  .answers      = ans->cells,
              });
        });
  }
}

} // namespace zigzag::vortex::logic

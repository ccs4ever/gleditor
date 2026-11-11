#include "common/xanadu/vpl/execution.hpp"

#include <memory>
#include <unordered_set>

#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vpl {
namespace {

class CursorReset {
public:
  CursorReset(zigzag::vortex::VortexVM &vm, zigzag::CellRef cursor)
      : vm_(vm), cursor_(cursor) {}
  ~CursorReset() { vm_.setCursorOpcode(cursor_, std::nullopt); }

  CursorReset(const CursorReset &)            = delete;
  CursorReset &operator=(const CursorReset &) = delete;

private:
  zigzag::vortex::VortexVM &vm_;
  zigzag::CellRef cursor_;
};

// A rank view carries the first cell's number as a scalar payload and its
// measured extents: the oracle's `mintRank` did, and `⍴A` reads its answer as
// `scalarInt()` while the result is still a rank of one cell.
VplView makeRankView(std::optional<zigzag::CellRef> origin,
                     const std::vector<zigzag::DirectedDim> &axes,
                     zigzag::ArenaManifold &arena) {
  VplView view{origin, axes};
  if (!origin) return view;
  if (const auto value = arena.asInt64(*origin)) {
    view.setScalarPayload(*value);
  } else if (const auto value = arena.asDouble(*origin)) {
    view.setScalarPayload(*value);
  }
  view.setExtents(view.shape(arena));
  return view;
}

struct ViewFacts {
  std::vector<zigzag::DirectedDim> axes;
  bool enclosed{false};
  const std::optional<std::vector<zigzag::DirectedDim>> *dimensionValues{
      nullptr};
};

VplView makeView(std::optional<zigzag::CellRef> cell, const ViewFacts &facts,
                 zigzag::vortex::VortexCore &core) {
  auto &arena = core.arena();
  // A dimension is compile-time structure: no cells, only the axes.
  if (facts.dimensionValues != nullptr && facts.dimensionValues->has_value()) {
    return VplView{std::nullopt, **facts.dimensionValues};
  }
  // An empty array has axes and no origin: its register was cleared.
  if (!cell) return VplView{std::nullopt, facts.axes};

  if (facts.enclosed) {
    const auto dimension = core.findOrMintDimension(enclosureDimensionName);
    const auto content = arena.linked(*cell, dimension, zigzag::DimVector::POS);
    VplView box{cell, facts.axes};
    box.enclose(
        std::make_shared<VplView>(makeRankView(content, facts.axes, arena)));
    return box;
  }
  if (!facts.axes.empty()) return makeRankView(cell, facts.axes, arena);

  if (const auto value = arena.asInt64(*cell)) {
    return VplView::makeScalar(*value, std::optional{*cell});
  }
  if (const auto value = arena.asDouble(*cell)) {
    return VplView::makeScalar(*value, true, *cell);
  }
  if (const auto value = arena.asBool(*cell)) {
    return VplView::makeScalar(static_cast<std::int64_t>(*value),
                               std::optional{*cell});
  }
  return VplView::makeScalar(arena.textOf(*cell), *cell);
}

void addDependency(std::vector<zigzag::CellRef> &dependencies,
                   zigzag::CellRef dependency) {
  if (std::ranges::find(dependencies, dependency) == dependencies.end()) {
    dependencies.push_back(dependency);
  }
}

} // namespace

std::expected<ExecutionResult, ExecutionError>
Executor::execute(std::string_view source, std::size_t maxCycles,
                  bool optimize) {
  if (&vm_.core() != &core_) {
    return std::unexpected(ExecutionError{
        ExecutionError::Stage::Configuration,
        "VPL executor core and VM must belong to the same runtime"});
  }
  if (maxCycles == 0) {
    return std::unexpected(
        ExecutionError{ExecutionError::Stage::Budget,
                       "VPL execution cycle budget must be positive"});
  }
  try {
    VPLCompiler compiler(core_, vm_);
    auto compiled = compiler.compile(
        source, {.optimize = optimize, .initialBindings = bindings_});
    return run(std::move(compiled), maxCycles);
  } catch (const std::exception &error) {
    return std::unexpected(
        ExecutionError{ExecutionError::Stage::Compile, error.what()});
  }
}

std::expected<ExecutionResult, ExecutionError>
Executor::execute(const AstNode &ast, std::size_t maxCycles, bool optimize) {
  if (&vm_.core() != &core_) {
    return std::unexpected(ExecutionError{
        ExecutionError::Stage::Configuration,
        "VPL executor core and VM must belong to the same runtime"});
  }
  if (maxCycles == 0) {
    return std::unexpected(
        ExecutionError{ExecutionError::Stage::Budget,
                       "VPL execution cycle budget must be positive"});
  }
  try {
    VPLCompiler compiler(core_, vm_);
    auto compiled = compiler.compile(
        ast, {.optimize = optimize, .initialBindings = bindings_});
    return run(std::move(compiled), maxCycles);
  } catch (const std::exception &error) {
    return std::unexpected(
        ExecutionError{ExecutionError::Stage::Compile, error.what()});
  }
}

std::expected<ExecutionResult, ExecutionError>
Executor::run(CompilationResult compiled, std::size_t maxCycles) {
  if (!compiled.success) {
    return std::unexpected(ExecutionError{ExecutionError::Stage::Compile,
                                          std::move(compiled.errorMessage)});
  }
  if (!compiled.cursor || compiled.resultSlots.size() != 1) {
    return std::unexpected(ExecutionError{
        ExecutionError::Stage::Compile,
        "Compiled VPL program has no executable result or cursor"});
  }
  if (const auto unplaced =
          unplacedBranchTarget(vm_, compiled.generatedOpcodes)) {
    return std::unexpected(
        ExecutionError{ExecutionError::Stage::Compile, *unplaced});
  }

  const auto cursor = *compiled.cursor;
  try {
    vm_.setCursorOpcode(cursor, compiled.entryOpcode);
    CursorReset reset(vm_, cursor);
    const auto execution = vm_.run(cursor, maxCycles);
    if (!execution.success) {
      const auto stage = execution.errorMessage == "Exceeded max cycles"
                             ? ExecutionError::Stage::Budget
                             : ExecutionError::Stage::Runtime;
      return std::unexpected(
          ExecutionError{stage, std::move(execution.errorMessage)});
    }

    auto &arena           = core_.arena();
    const auto resultCell = vm_.resultCell(compiled.resultSlots.front());
    // A cleared result register is an empty array when the program said the
    // result has axes, and a failure otherwise.
    if (!resultCell && compiled.resultAxes.empty() &&
        !compiled.resultDimensionValues) {
      return std::unexpected(ExecutionError{
          ExecutionError::Stage::Runtime,
          "VPL program completed without producing its result cell"});
    }

    auto resultDependencies  = compiled.resultDependencies;
    const bool foreignResult = resultCell && arena.resolveForeign(*resultCell);
    if (foreignResult) addDependency(resultDependencies, *resultCell);
    auto view = makeView(resultCell,
                         {.axes            = compiled.resultAxes,
                          .enclosed        = compiled.resultEnclosed,
                          .dimensionValues = &compiled.resultDimensionValues},
                         core_);
    view.setDependencies(resultDependencies);

    auto updatedBindings     = bindings_;
    auto updatedDependencies = dependencies_;
    for (auto &[cell, cellDependencies] : compiled.cellDependencies) {
      updatedDependencies.insert_or_assign(cell, std::move(cellDependencies));
    }
    if (foreignResult) {
      addDependency(updatedDependencies[*resultCell], *resultCell);
    }
    // Cells the program made at run time have no compile-time facts, so each
    // takes its value's union of operands -- unless it already has its own,
    // which try_emplace leaves alone (the oracle's rule).
    const auto recordUnion = [&](const VplView &value,
                                 const std::vector<zigzag::CellRef> &sources) {
      if (sources.empty() || value.axes().empty() || !value.origin()) return;
      for (const auto member : value.collectCells(arena)) {
        arena.consumeWork();
        updatedDependencies.try_emplace(member, sources);
      }
    };
    recordUnion(view, resultDependencies);
    for (const auto &[name, binding] : compiled.assignedBindings) {
      const auto cell = vm_.resultCell(binding.cell);
      const bool hasStructure =
          binding.dimensionValues.has_value() || !binding.axes.empty();
      if (!cell && !hasStructure) {
        return std::unexpected(ExecutionError{
            ExecutionError::Stage::Runtime,
            "VPL assignment did not produce a value for " + name});
      }
      auto bindingDependencies = binding.dependencies;
      if (cell && arena.resolveForeign(*cell)) {
        addDependency(bindingDependencies, *cell);
        addDependency(updatedDependencies[*cell], *cell);
      }
      VplView assigned = makeView(cell,
                                  {.axes            = binding.axes,
                                   .enclosed        = binding.enclosed,
                                   .dimensionValues = &binding.dimensionValues},
                                  core_);
      recordUnion(assigned, bindingDependencies);
      assigned.setDependencies(std::move(bindingDependencies));
      updatedBindings.insert_or_assign(name, std::move(assigned));
    }
    bindings_.swap(updatedBindings);
    dependencies_.swap(updatedDependencies);
    return ExecutionResult{.view        = std::move(view),
                           .disassembly = std::move(compiled.disassembly),
                           .print       = compiled.resultPrint};
  } catch (const std::exception &error) {
    return std::unexpected(
        ExecutionError{ExecutionError::Stage::Runtime, error.what()});
  }
}

std::optional<std::string>
unplacedBranchTarget(const zigzag::vortex::VortexVM &vm,
                     std::span<const zigzag::CellRef> chain) {
  using zigzag::vortex::OpcodeKind;
  const std::unordered_set<zigzag::CellRef> placed(chain.begin(), chain.end());
  for (const auto op : chain) {
    const auto kind = vm.getOpcodeKind(op);
    if (!kind ||
        (*kind != OpcodeKind::BranchFalseCell && *kind != OpcodeKind::Branch)) {
      continue;
    }
    const auto inputs = vm.core().inputsOf(op);
    if (inputs.size() < 2) continue;
    if (!placed.contains(inputs[1])) {
      return "VPL graph branch " + std::to_string(op) + " targets label " +
             std::to_string(inputs[1]) + " that was never placed";
    }
  }
  return std::nullopt;
}

Executor *Executor::setVariable(std::string_view name, VplView value) {
  if (const auto origin = value.origin()) {
    dependencies_.insert_or_assign(*origin, value.dependencies());
  }
  // A foreign cell is its own source: record that now, before any program
  // reads it, so a later calculation can name it.
  auto sources = value.dependencies();
  for (const auto cell : value.collectCells(core_.arena())) {
    core_.arena().consumeWork();
    if (core_.arena().resolveForeign(cell)) {
      addDependency(sources, cell);
      dependencies_.try_emplace(cell, std::vector{cell});
    }
  }
  value.setDependencies(std::move(sources));
  bindings_.insert_or_assign(std::string(name), std::move(value));
  return this;
}

std::optional<VplView> Executor::getVariable(std::string_view name) const {
  const auto it = bindings_.find(std::string(name));
  if (it == bindings_.end()) return std::nullopt;
  return it->second;
}

zigzag::DimRef Executor::resolveDimension(std::string_view name) {
  VPLCompiler compiler(core_, vm_);
  return compiler.resolveDimension(name);
}

std::span<const zigzag::CellRef>
Executor::dependenciesOf(zigzag::CellRef cell) const noexcept {
  const auto it = dependencies_.find(cell);
  if (it == dependencies_.end()) return {};
  return it->second;
}

} // namespace xanadu::vpl

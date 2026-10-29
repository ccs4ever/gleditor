#include "common/xanadu/vpl/execution.hpp"

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

VplView makeView(zigzag::CellRef cell,
                 const std::vector<zigzag::DirectedDim> &axes,
                 zigzag::ArenaManifold &arena) {
  VplView view{cell, axes};
  if (!axes.empty()) return view;

  if (const auto value = arena.asInt64(cell)) {
    return VplView::makeScalar(*value, std::optional{cell});
  }
  if (const auto value = arena.asDouble(cell)) {
    return VplView::makeScalar(*value, true, cell);
  }
  if (const auto value = arena.asBool(cell)) {
    return VplView::makeScalar(static_cast<std::int64_t>(*value),
                               std::optional{cell});
  }
  return VplView::makeScalar(arena.textOf(cell), cell);
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

    const auto resultCell = vm_.resultCell(compiled.resultSlots.front());
    if (!resultCell) {
      return std::unexpected(ExecutionError{
          ExecutionError::Stage::Runtime,
          "VPL program completed without producing its result cell"});
    }

    auto &arena             = core_.arena();
    auto resultDependencies = compiled.resultDependencies;
    if (arena.resolveForeign(*resultCell)) {
      addDependency(resultDependencies, *resultCell);
    }
    auto view = makeView(*resultCell, compiled.resultAxes, arena);
    view.setDependencies(resultDependencies);

    auto updatedBindings     = bindings_;
    auto updatedDependencies = dependencies_;
    for (auto &[cell, cellDependencies] : compiled.cellDependencies) {
      updatedDependencies.insert_or_assign(cell, std::move(cellDependencies));
    }
    if (arena.resolveForeign(*resultCell)) {
      auto &cellDependencies = updatedDependencies[*resultCell];
      addDependency(cellDependencies, *resultCell);
    }
    for (const auto &[name, binding] : compiled.assignedBindings) {
      const auto cell = vm_.resultCell(binding.cell);
      if (!cell) {
        return std::unexpected(ExecutionError{
            ExecutionError::Stage::Runtime,
            "VPL assignment did not produce a value for " + name});
      }
      auto bindingDependencies = binding.dependencies;
      if (arena.resolveForeign(*cell)) {
        addDependency(bindingDependencies, *cell);
        addDependency(updatedDependencies[*cell], *cell);
      }
      VplView assigned = makeView(*cell, binding.axes, arena);
      assigned.setDependencies(std::move(bindingDependencies));
      updatedBindings.insert_or_assign(name, std::move(assigned));
    }
    bindings_.swap(updatedBindings);
    dependencies_.swap(updatedDependencies);
    return ExecutionResult{.view        = std::move(view),
                           .disassembly = std::move(compiled.disassembly)};
  } catch (const std::exception &error) {
    return std::unexpected(
        ExecutionError{ExecutionError::Stage::Runtime, error.what()});
  }
}

Executor *Executor::setVariable(std::string_view name, VplView value) {
  if (const auto origin = value.origin()) {
    dependencies_.insert_or_assign(*origin, value.dependencies());
  }
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

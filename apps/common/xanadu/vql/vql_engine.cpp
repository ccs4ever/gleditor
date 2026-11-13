/**
 * @file vql_engine.cpp
 * @brief Direct query evaluation engine for VQL v16.0.
 */
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/functor_capture.hpp"
#include "common/xanadu/vql/invocation.hpp"
#include "common/xanadu/zigzag/query_clone_master.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <concepts>
#include <format>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include <gleditor/cpp26_concat.hpp>

namespace xanadu::vql {
using zigzag::DimVector;

namespace {

template <class Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

bool stringToDouble(std::string_view sv, double &out) {
  if (sv.empty()) {
    return false;
  }
  std::string s(sv);
  char *end = nullptr;
  out       = std::strtod(s.c_str(), &end);
  return end == s.c_str() + s.size();
}

double cellValueToDouble(const zigzag::vortex::CellValue &val,
                         bool &convertible) {
  convertible = true;
  if (std::holds_alternative<double>(val)) {
    return std::get<double>(val);
  }
  if (std::holds_alternative<std::int64_t>(val)) {
    return static_cast<double>(std::get<std::int64_t>(val));
  }
  if (std::holds_alternative<bool>(val)) {
    return std::get<bool>(val) ? 1.0 : 0.0;
  }
  if (std::holds_alternative<std::string>(val)) {
    double d = 0.0;
    if (stringToDouble(std::get<std::string>(val), d)) {
      return d;
    }
  }
  convertible = false;
  return 0.0;
}

} // namespace

VQLEngine::VQLEngine(MultiStoreCoordinator &coordinator)
    : coordinator_(coordinator), core_(&coordinator.core()),
      contracts_(*core_, coordinator_) {}

VQLEngine::VQLEngine(zigzag::vortex::VortexCore &core)
    : ownedCoordinator_(std::make_unique<MultiStoreCoordinator>(core)),
      coordinator_(*ownedCoordinator_), core_(&core),
      contracts_(*core_, coordinator_) {}

VQLEngine::VQLEngine(zigzag::ArenaManifold &arena)
    : ownedCore_(std::make_unique<zigzag::vortex::VortexCore>(arena)),
      ownedCoordinator_(std::make_unique<MultiStoreCoordinator>(*ownedCore_)),
      coordinator_(*ownedCoordinator_), core_(ownedCore_.get()),
      contracts_(*core_, coordinator_) {}

namespace {
/// A value as the text a comparison or a search reads.
std::string textOfValue(const zigzag::vortex::CellValue &value) {
  return std::visit(
      [](const auto &held) -> std::string {
        using T = std::remove_cvref_t<decltype(held)>;
        if constexpr (std::same_as<T, std::string>) {
          return held;
        } else if constexpr (std::same_as<T, bool>) {
          return held ? "true" : "false";
        } else {
          return std::format("{}", held);
        }
      },
      value);
}
} // namespace

zigzag::DimRef VQLEngine::resolveDimension(std::string_view name) {
  return coordinator_.resolveDimension(name);
}

void VQLEngine::setVariable(std::string_view name, zigzag::CellRef value) {
  bindName(std::string(name), {value});
}

void VQLEngine::setVariable(std::string_view name,
                            std::vector<zigzag::CellRef> values) {
  bindName(std::string(name), std::move(values));
}

std::vector<zigzag::CellRef>
VQLEngine::getVariable(std::string_view name) const {
  auto it = env_.find(std::string(name));
  if (it != env_.end()) {
    return it->second;
  }
  return {};
}

void VQLEngine::clearVariables() noexcept {
  env_.clear();
  functors_.clear();
  putGetLenses_.clear();
}

std::unordered_map<std::string, std::vector<PathStep>>
VQLEngine::compilerFunctors() const {
  std::unordered_map<std::string, std::vector<PathStep>> result;
  result.reserve(functors_.size());
  for (const auto &[name, steps] : functors_)
    if (steps) result.emplace(name, *steps);
  return result;
}

void VQLEngine::publishMount(const std::string &dimName, SessionMount mount) {
  const std::string canonical =
      dimName.starts_with("d.") ? dimName : "d." + dimName;
  sessionMounts_[canonical.substr(2)] = mount;
  sessionMounts_[canonical]           = std::move(mount);
}

void VQLEngine::publishUnmount(const std::string &dimName) {
  const std::string canonical =
      dimName.starts_with("d.") ? dimName : "d." + dimName;
  sessionMounts_.erase(canonical);
  sessionMounts_.erase(canonical.substr(2));
}

void VQLEngine::publishCompilerBindings(
    ContractBindings bindings,
    std::unordered_map<std::string, std::vector<PathStep>> functors) {
  auto nextEnv      = env_;
  auto nextFunctors = functors_;
  auto nextLenses   = putGetLenses_;
  for (auto &[name, cells] : bindings) {
    nextFunctors.erase(name);
    nextLenses.erase(name);
    nextEnv[name] = std::move(cells);
  }
  for (auto &[name, steps] : functors) {
    nextEnv.erase(name);
    nextLenses.erase(name);
    nextFunctors[name] =
        std::make_shared<const std::vector<PathStep>>(std::move(steps));
  }
  env_.swap(nextEnv);
  functors_.swap(nextFunctors);
  putGetLenses_.swap(nextLenses);
}

namespace {

/// The one place a QueryError becomes an exception.
[[noreturn]] void throwQueryError(const QueryError &error) {
  switch (error.category) {
  case QueryErrorCategory::TypeContract:
    throw TypeContractError(error.message);
  case QueryErrorCategory::Parse:
    throw ParseError(error.message, SourceLocation{});
  case QueryErrorCategory::Malformed:
    throw std::invalid_argument(error.message);
  case QueryErrorCategory::Unsupported:
  case QueryErrorCategory::Unknown:
  case QueryErrorCategory::Runtime:
  case QueryErrorCategory::Budget:
    break;
  }
  throw QueryRefusal(error.category, error.message);
}

std::vector<zigzag::CellRef>
valueOrThrow(std::expected<std::vector<zigzag::CellRef>, QueryError> result) {
  if (!result) throwQueryError(result.error());
  return std::move(*result);
}

} // namespace

std::expected<std::vector<zigzag::CellRef>, QueryError>
VQLEngine::tryExecute(std::string_view queryString) {
  try {
    Parser parser(queryString);
    return tryExecute(parser.parseQuery());
  } catch (const ParseError &error) {
    return std::unexpected(QueryError{.category = QueryErrorCategory::Parse,
                                      .message  = error.what()});
  }
}

std::expected<std::vector<zigzag::CellRef>, QueryError>
VQLEngine::tryExecute(const QueryExpression &query) {
  zigzag::vortex::VortexVM vm(*core_);
  QueryExecutor executor(*this, vm);
  return executor.execute(query);
}

std::vector<zigzag::CellRef> VQLEngine::execute(std::string_view queryString) {
  // Parsed here, not in tryExecute, so a ParseError keeps its location.
  Parser parser(queryString);
  QueryExpression query = parser.parseQuery();
  return execute(query);
}

std::vector<zigzag::CellRef> VQLEngine::execute(const QueryExpression &query) {
  return valueOrThrow(tryExecute(query));
}

std::vector<zigzag::CellRef>
VQLEngine::executeScriptBody(const ScriptBody &body) {
  std::vector<zigzag::CellRef> lastResult;
  for (const auto &stmt : body.statements) {
    this->arena().consumeWork();
    auto res   = executeStatement(stmt);
    lastResult = std::move(res);
  }
  return lastResult;
}

bool VQLEngine::matchDestructure(const DestructurePattern &pattern,
                                 std::optional<zigzag::CellRef> context,
                                 Bindings &scopedBindings,
                                 const MissingField missing) {
  const auto evaluateDefault =
      [this](const ValueExpr &expr, const std::optional<zigzag::CellRef> ctx) {
        return cellsOfValueExpr(expr, ctx);
      };
  return contracts_.matchDestructure(pattern, context, scopedBindings, missing,
                                     evaluateDefault);
}

bool VQLEngine::satisfiesType(const TypeExpr &type,
                              const std::span<const zigzag::CellRef> cells,
                              Bindings *const scopedBindings) {
  const auto evaluateDefault =
      [this](const ValueExpr &expr, const std::optional<zigzag::CellRef> ctx) {
        return cellsOfValueExpr(expr, ctx);
      };
  return contracts_.satisfiesType(type, cells, scopedBindings, evaluateDefault);
}

std::vector<zigzag::CellRef>
VQLEngine::cellsOfValueExpr(const ValueExpr &expr,
                            const std::optional<zigzag::CellRef> context) {
  if (const auto *version =
          std::get_if<std::shared_ptr<VersionExpr>>(&expr.kind)) {
    const auto resolved = evaluateVersionExpr(**version, context);
    if (!resolved) return {};
    const auto cell = arena().makeCell(resolved->str());
    versionValues_.insert_or_assign(cell, *resolved);
    return {cell};
  }
  std::vector<zigzag::CellRef> contextCells;
  if (context) contextCells.push_back(*context);
  if (const auto *shape =
          std::get_if<std::shared_ptr<ShapeLiteral>>(&expr.kind)) {
    return *shape ? std::vector<zigzag::CellRef>{makeShapeHandle(**shape)}
                  : std::vector<zigzag::CellRef>{};
  }
  if (const auto *path =
          std::get_if<std::shared_ptr<PathExpression>>(&expr.kind)) {
    return evaluatePath(**path, contextCells);
  }
  if (const auto *name = std::get_if<std::string>(&expr.kind)) {
    if (const auto functor = functorNamed(*name)) {
      return applyFunctor(**functor, std::move(contextCells));
    }
    auto it = env_.find(*name);
    return it != env_.end() ? it->second : std::vector<zigzag::CellRef>{};
  }
  if (const auto *function =
          std::get_if<std::shared_ptr<FunctionInvocation>>(&expr.kind)) {
    if (*function) {
      if (const auto found = userDefs_.find((*function)->name);
          found != userDefs_.end()) {
        requirePositionalArguments(**function);
        return callUserDef(found->second, (*function)->args, context);
      }
    }
  }
  if (const auto value = evaluateValueExpr(expr, context)) {
    return {cellFromValue(*value)};
  }
  return {};
}

std::optional<VQLEngine::FunctorSteps>
VQLEngine::functorNamed(const std::string_view name) const {
  const auto it = functors_.find(std::string(name));
  if (it == functors_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<VQLEngine::FunctorSteps>
VQLEngine::functorOf(const PathExpression &path) const {
  const bool existingFunctorAnchor = path.anchor.kind == AnchorKind::Variable &&
                                     functorNamed(path.anchor.name).has_value();
  const bool functorExpression = path.isFunctor || existingFunctorAnchor;
  if (functorExpression) {
    if (path.anchorProjection || path.shapeProjection)
      throw QueryRefusal(
          QueryErrorCategory::Unsupported,
          "ordered shape projections are not supported in functors");
    validateFunctorCaptureSteps(path.steps);
  }
  if (path.anchorProjection || path.shapeProjection ||
      std::ranges::any_of(path.steps, [](const PathStep &step) {
        return step.projectionAfter.has_value();
      })) {
    return std::nullopt;
  }
  if (!path.anchorPredicates.empty() || path.cloneTail) {
    return std::nullopt;
  }
  std::vector<PathStep> steps;
  if (!path.isFunctor) {
    const auto &anchor = path.anchor;
    if (anchor.kind != AnchorKind::Variable || anchor.versionPin ||
        anchor.derefMaster) {
      return std::nullopt;
    }
    const auto head = functorNamed(anchor.name);
    if (!head) {
      return std::nullopt;
    }
    steps = **head;
  }
  auto captured = captureFunctorApplications(
      path.steps,
      [&](std::string_view name) -> std::optional<std::vector<PathStep>> {
        const auto found = functorNamed(name);
        if (!found) return std::nullopt;
        return **found;
      });
  steps.insert(steps.end(), captured.begin(), captured.end());
  return std::make_shared<const std::vector<PathStep>>(std::move(steps));
}

std::vector<zigzag::CellRef>
VQLEngine::applyFunctor(const std::vector<PathStep> &steps,
                        std::vector<zigzag::CellRef> cells) {
  for (const auto &step : steps) {
    this->arena().consumeWork();
    cells = evaluateStep(step, cells);
  }
  return cells;
}

std::vector<zigzag::CellRef> VQLEngine::bindLet(const LetClause &let) {
  if (bindLens(let)) return {};
  putGetLenses_.erase(let.varName);
  const PathExpression *path = std::get_if<PathExpression>(&let.target);
  const ValueExpr *value     = std::get_if<ValueExpr>(&let.target);
  if (value != nullptr) {
    if (const auto *inner =
            std::get_if<std::shared_ptr<PathExpression>>(&value->kind)) {
      path = inner->get();
    }
  }
  std::optional<FunctorSteps> functor;
  if (path != nullptr) {
    functor = functorOf(*path);
  } else if (const auto *name = std::get_if<std::string>(&value->kind)) {
    functor = functorNamed(*name);
  }
  if (functor) {
    env_.erase(let.varName);
    functors_[let.varName] = std::move(*functor);
    return {};
  }
  functors_.erase(let.varName);
  auto cells        = path != nullptr ? evaluatePath(*path)
                                      : cellsOfValueExpr(*value, std::nullopt);
  env_[let.varName] = cells;
  return cells;
}

void VQLEngine::bindName(const std::string &name,
                         std::vector<zigzag::CellRef> cells) {
  functors_.erase(name);
  putGetLenses_.erase(name);
  env_[name] = std::move(cells);
}

std::vector<zigzag::CellRef>
VQLEngine::executeLambda(const PathLambda &lambda,
                         std::optional<zigzag::CellRef> context) {
  this->arena().consumeWork();
  auto savedEnv      = env_;
  auto savedFunctors = functors_;
  auto savedLenses   = putGetLenses_;
  Bindings scoped;
  bool restored      = false;
  const auto restore = [&] {
    if (restored) return;
    restored      = true;
    env_          = std::move(savedEnv);
    functors_     = std::move(savedFunctors);
    putGetLenses_ = std::move(savedLenses);
  };
  const RestoreScope restoreOnExit{restore};

  if (!lambda.params.empty()) {
    const auto &param = lambda.params[0];
    if (std::holds_alternative<DestructurePattern>(param.target)) {
      if (!matchDestructure(std::get<DestructurePattern>(param.target), context,
                            scoped)) {
        restore();
        return {};
      }
    } else {
      const auto given = context
                             ? std::span<const zigzag::CellRef>{&*context, 1}
                             : std::span<const zigzag::CellRef>{};
      if (param.type && !satisfiesType(*param.type, given, &scoped)) {
        restore();
        return {};
      }
      if (std::holds_alternative<bool>(param.target)) {
        scoped["."] = context ? std::vector<zigzag::CellRef>{*context}
                              : std::vector<zigzag::CellRef>{};
      } else {
        scoped[std::get<std::string>(param.target)] =
            context ? std::vector<zigzag::CellRef>{*context}
                    : std::vector<zigzag::CellRef>{};
      }
    }
  } else {
    scoped["."] = context ? std::vector<zigzag::CellRef>{*context}
                          : std::vector<zigzag::CellRef>{};
  }

  for (auto &[k, v] : scoped) {
    bindName(k, std::move(v));
  }

  std::vector<zigzag::CellRef> results;
  if (std::holds_alternative<std::shared_ptr<ScriptBody>>(lambda.body)) {
    results =
        executeScriptBody(*std::get<std::shared_ptr<ScriptBody>>(lambda.body));
  } else if (std::holds_alternative<std::shared_ptr<ValueExpr>>(lambda.body)) {
    results = cellsOfValueExpr(
        *std::get<std::shared_ptr<ValueExpr>>(lambda.body), context);
  }

  restore();
  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::callUserDef(const DefDeclaration &def,
                       const std::vector<ValueExpr> &args,
                       std::optional<zigzag::CellRef> context) {
  this->arena().consumeWork();
  auto savedEnv      = env_;
  auto savedFunctors = functors_;
  auto savedLenses   = putGetLenses_;
  bool restored      = false;
  const auto restore = [&] {
    if (restored) return;
    restored      = true;
    env_          = std::move(savedEnv);
    functors_     = std::move(savedFunctors);
    putGetLenses_ = std::move(savedLenses);
  };
  const RestoreScope restoreOnExit{restore};

  struct PreparedArgument {
    std::vector<zigzag::CellRef> cells;
    std::optional<FunctorSteps> functor;
  };
  std::vector<PreparedArgument> prepared;
  prepared.reserve(args.size());
  for (std::size_t index = 0; index < args.size(); ++index) {
    const ValueExpr &arg         = args[index];
    const LambdaParam *parameter = nullptr;
    std::size_t parameterIndex   = 0;
    for (const auto &candidate : def.params) {
      if (candidate.isVariadic) {
        if (index >= parameterIndex) parameter = &candidate;
        break;
      }
      if (index == parameterIndex) {
        parameter = &candidate;
        break;
      }
      ++parameterIndex;
    }

    PreparedArgument value;
    if (parameter && !parameter->isVariadic && !parameter->type &&
        std::holds_alternative<std::string>(parameter->target)) {
      if (const auto *path =
              std::get_if<std::shared_ptr<PathExpression>>(&arg.kind);
          path && *path) {
        value.functor = functorOf(**path);
      } else if (const auto *name = std::get_if<std::string>(&arg.kind)) {
        value.functor = functorNamed(*name);
      }
    }
    if (!value.functor) value.cells = cellsOfValueExpr(arg, context);
    prepared.push_back(std::move(value));
  }

  std::size_t argIdx = 0;
  for (std::size_t i = 0; i < def.params.size(); ++i) {
    const auto &param = def.params[i];
    if (param.isVariadic) {
      std::vector<zigzag::CellRef> restCells;
      while (argIdx < args.size()) {
        auto cells = std::move(prepared[argIdx++].cells);
        // A variadic's type is each argument's, not the whole tail's.
        if (param.type && !satisfiesType(*param.type, cells, nullptr)) {
          restore();
          return {};
        }
        restCells.insert(restCells.end(), cells.begin(), cells.end());
      }
      if (std::holds_alternative<std::string>(param.target)) {
        bindName(std::get<std::string>(param.target), std::move(restCells));
      }
      break;
    }

    const PreparedArgument *arg =
        argIdx < args.size() ? &prepared[argIdx++] : nullptr;

    // An untyped name parameter takes a functor argument as a functor, so a
    // traversal can be handed to a def and applied there.
    if (arg != nullptr && arg->functor) {
      const auto &name = std::get<std::string>(param.target);
      env_.erase(name);
      functors_[name] = std::move(*arg->functor);
      continue;
    }

    std::vector<zigzag::CellRef> paramCells;
    if (arg != nullptr) {
      paramCells = arg->cells;
    } else if (param.defaultValue) {
      paramCells = cellsOfValueExpr(*param.defaultValue, context);
    }

    Bindings scoped;
    if (std::holds_alternative<DestructurePattern>(param.target)) {
      const std::optional<zigzag::CellRef> targetC =
          paramCells.empty()
              ? std::nullopt
              : std::optional<zigzag::CellRef>{paramCells.front()};
      if (!matchDestructure(std::get<DestructurePattern>(param.target), targetC,
                            scoped)) {
        restore();
        return {};
      }
    } else {
      if (param.type && !satisfiesType(*param.type, paramCells, &scoped)) {
        restore();
        return {};
      }
      if (std::holds_alternative<bool>(param.target)) {
        scoped["."] = std::move(paramCells);
      } else {
        scoped[std::get<std::string>(param.target)] = std::move(paramCells);
      }
    }
    for (auto &[k, v] : scoped) {
      bindName(k, std::move(v));
    }
  }

  std::vector<zigzag::CellRef> results;
  if (std::holds_alternative<std::shared_ptr<ScriptBody>>(def.body)) {
    results =
        executeScriptBody(*std::get<std::shared_ptr<ScriptBody>>(def.body));
  } else if (std::holds_alternative<std::shared_ptr<ValueExpr>>(def.body)) {
    results = cellsOfValueExpr(*std::get<std::shared_ptr<ValueExpr>>(def.body),
                               context);
  }

  restore();
  // The parameters are a match and the return type a promise: a def whose
  // body breaks its own signature is a bug to report, not a cell to skip.
  if (def.returnType && !satisfiesType(*def.returnType, results, nullptr)) {
    throw TypeContractError(def.name +
                            "() returned a value outside its declared type");
  }
  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::executeStatement(const Statement &stmt) {
  QueryExpression query;
  query.body.statements.push_back(stmt);
  return execute(query);
}

std::vector<VQLEngine::WatchEvent> VQLEngine::pollWatch(const WatchId id) {
  return pollWatch(id, [](const WatchEvent &) {});
}

std::vector<VQLEngine::WatchEvent> VQLEngine::pollWatch(
    const WatchId id,
    const gleditor::cpp26::function_ref<void(const WatchEvent &)> onEvent) {
  const auto found = watches_.find(id);
  if (found == watches_.end())
    throw std::out_of_range("unknown VQL watch subscription");
  if (found->second.polling)
    throw std::logic_error("VQL watch subscription is already being polled");

  const PathExpression path = found->second.path;
  const auto current        = evaluatePath(path);
  std::unordered_map<zigzag::CellRef, WatchSubscription::Snapshot> next;
  std::vector<zigzag::CellRef> nextOrder;
  next.reserve(current.size());
  nextOrder.reserve(current.size());
  for (const auto cell : current) {
    WatchSubscription::Snapshot snapshot{.value    = core_->render(cell),
                                         .topology = watchTopology(cell)};
    if (next.emplace(cell, std::move(snapshot)).second)
      nextOrder.push_back(cell);
  }
  std::vector<WatchEvent> events;
  for (const auto cell : nextOrder) {
    const auto &snapshot = next.at(cell);
    const auto previous  = found->second.previous.find(cell);
    if (previous == found->second.previous.end()) {
      events.push_back({.cell       = cell,
                        .change     = WatchChange::Added,
                        .afterValue = snapshot.value});
      continue;
    }
    const bool valueChanged    = previous->second.value != snapshot.value;
    const bool topologyChanged = previous->second.topology != snapshot.topology;
    if (valueChanged || topologyChanged) {
      events.push_back({.cell            = cell,
                        .change          = WatchChange::Modified,
                        .beforeValue     = previous->second.value,
                        .afterValue      = snapshot.value,
                        .topologyChanged = topologyChanged});
    }
  }
  for (const auto cell : found->second.order) {
    const auto previous = found->second.previous.find(cell);
    if (previous != found->second.previous.end() && !next.contains(cell)) {
      events.push_back({.cell        = cell,
                        .change      = WatchChange::Removed,
                        .beforeValue = previous->second.value});
    }
  }
  found->second.polling = true;
  try {
    for (const auto &event : events) onEvent(event);
  } catch (...) {
    if (const auto active = watches_.find(id); active != watches_.end())
      active->second.polling = false;
    throw;
  }

  // Advance after callbacks so writes caused by this batch are part of the
  // next baseline and cannot recursively trigger the same subscription.
  auto active = watches_.find(id);
  if (active == watches_.end()) return events;
  try {
    const auto settled = evaluatePath(path);
    next.clear();
    nextOrder.clear();
    for (const auto cell : settled) {
      WatchSubscription::Snapshot snapshot{.value    = core_->render(cell),
                                           .topology = watchTopology(cell)};
      if (next.emplace(cell, std::move(snapshot)).second)
        nextOrder.push_back(cell);
    }
  } catch (...) {
    if (const auto stillActive = watches_.find(id);
        stillActive != watches_.end()) {
      stillActive->second.polling = false;
    }
    throw;
  }
  active->second.previous = std::move(next);
  active->second.order    = std::move(nextOrder);
  active->second.polling  = false;
  return events;
}

bool VQLEngine::cancelWatch(const WatchId id) noexcept {
  return watches_.erase(id) != 0;
}

VQLEngine::WatchSourceKey
VQLEngine::watchSourceKey(const zigzag::CellRef cell) const {
  if (const auto source = arena().resolveSourceCell(cell))
    return {source->first, source->second};
  return {nullptr, cell};
}

std::vector<VQLEngine::WatchTopologyLink>
VQLEngine::watchTopology(const zigzag::CellRef cell) {
  if (const auto foreign = arena().foreignOf(cell)) {
    const auto space = arena().spaceAt(foreign->space);
    if (space && space->manifold) {
      std::vector<WatchTopologyLink> topology;
      std::unordered_set<zigzag::DimRef> localDimensions;
      for (const auto foreignDim : space->manifold->dimensions()) {
        const auto arenaDim = arenaDimensionFor(cell, foreignDim);
        localDimensions.insert(arenaDim);
        const auto pos =
            arena().linkedMinting(cell, arenaDim, zigzag::DimVector::POS);
        const auto neg =
            arena().linkedMinting(cell, arenaDim, zigzag::DimVector::NEG);
        if (!pos && !neg) continue;
        topology.push_back(
            {.dim = {space->store, foreignDim},
             .pos = pos ? watchSourceKey(*pos)
                        : WatchSourceKey{nullptr, zigzag::noCell},
             .neg = neg ? watchSourceKey(*neg)
                        : WatchSourceKey{nullptr, zigzag::noCell}});
      }
      const auto localLinks = arena().dimensionsOf(cell);
      for (const auto &link : localLinks) {
        if (arena().dimIn(foreign->space, link.dim)) continue;
        if (!localDimensions.insert(link.dim).second) continue;
        const auto pos =
            arena().linkedMinting(cell, link.dim, zigzag::DimVector::POS);
        const auto neg =
            arena().linkedMinting(cell, link.dim, zigzag::DimVector::NEG);
        if (!pos && !neg) continue;
        topology.push_back(
            {.dim = watchSourceKey(link.dim),
             .pos = pos ? watchSourceKey(*pos)
                        : WatchSourceKey{nullptr, zigzag::noCell},
             .neg = neg ? watchSourceKey(*neg)
                        : WatchSourceKey{nullptr, zigzag::noCell}});
      }
      return topology;
    }
  }
  const auto links = arena().dimensionsOf(cell);
  std::vector<WatchTopologyLink> topology;
  topology.reserve(links.size());
  for (const auto &link : links) {
    topology.push_back({.dim = watchSourceKey(link.dim),
                        .pos = watchSourceKey(link.pos),
                        .neg = watchSourceKey(link.neg)});
  }
  return topology;
}

std::vector<zigzag::CellRef>
VQLEngine::resolveStoreAnchor(const StoreAnchor &sa) {
  std::vector<zigzag::CellRef> results;
  if (sa.method == "current") {
    if (const auto home = coordinator_.homeAnchor()) results.push_back(*home);
  } else if (sa.method == "local" && !sa.args.empty()) {
    const auto pathValue = evaluateValueExpr(sa.args.front(), std::nullopt);
    if (pathValue) {
      const auto path = textOfValue(*pathValue);
      for (const auto &info : coordinator_.stores()) {
        if (info.path == path || info.label == path) {
          if (info.homeCell) results.push_back(*info.homeCell);
          break;
        }
      }
    }
  } else if (sa.method == "system" && !sa.args.empty()) {
    const auto domainValue = evaluateValueExpr(sa.args.front(), std::nullopt);
    if (domainValue) {
      if (const auto home =
              coordinator_.resolveNamedStore(textOfValue(*domainValue)))
        results.push_back(*home);
    }
  } else if (sa.method.empty() && !sa.sliceName.empty()) {
    if (const auto sh = coordinator_.resolveNamedStore(sa.sliceName)) {
      results.push_back(*sh);
    }
  }
  if (sa.method.empty() && results.empty() && !sa.args.empty()) {
    const auto storeKeyValue = evaluateValueExpr(sa.args.front(), std::nullopt);
    if (!storeKeyValue) return results;
    const auto storeKey = textOfValue(*storeKeyValue);
    if (const auto sh = coordinator_.resolveNamedStore(storeKey)) {
      results.push_back(*sh);
    }
  }
  if (sa.method.empty() && results.empty()) {
    if (const auto h = coordinator_.homeAnchor()) {
      results.push_back(*h);
    }
  }
  if (sa.derefMaster) {
    for (auto &c : results) {
      this->arena().consumeWork();
      c = zigzag::queryCloneMaster(core_->arena(), c, core_->dims().clone)
              .value_or(c);
    }
  }
  return results;
}

std::optional<zigzag::CellRef> VQLEngine::sliceHomeOf(zigzag::CellRef cell) {
  if (!core_->arena().contains(cell)) {
    return std::nullopt;
  }
  if (const auto master = coordinator_.derefCloneMaster(cell)) {
    if (*master != cell) {
      return *master;
    }
  }
  for (const auto &info : coordinator_.stores()) {
    if (info.storeCell == cell && info.homeCell) {
      return *info.homeCell;
    }
    if (info.homeCell == cell) {
      return cell;
    }
  }
  return cell;
}

std::vector<zigzag::CellRef> VQLEngine::evaluatePerspectiveStep(
    std::string_view perspName, const PathStep &targetStep,
    const std::vector<zigzag::CellRef> &currentCells) {
  std::string normPersp(perspName);
  if (normPersp.starts_with('@')) {
    normPersp = normPersp.substr(1);
  }
  if (normPersp.ends_with(':')) {
    normPersp = normPersp.substr(0, normPersp.size() - 1);
  }
  if (normPersp.empty()) {
    normPersp = activePerspective_;
  }

  std::vector<zigzag::CellRef> results;
  for (zigzag::CellRef c : currentCells) {
    this->arena().consumeWork();
    std::optional<zigzag::CellRef> cloneCell;
    if (!normPersp.empty()) {
      auto itP = perspectiveClones_.find(normPersp);
      if (itP != perspectiveClones_.end()) {
        auto itC = itP->second.find(c);
        if (itC != itP->second.end()) {
          cloneCell = itC->second;
        }
      }
    }

    std::string saved  = activePerspective_;
    activePerspective_ = normPersp;

    std::vector<zigzag::CellRef> res;
    if (cloneCell) {
      res = evaluateStep(targetStep, {*cloneCell});
    }
    if (res.empty()) {
      res = evaluateStep(targetStep, {c});
    }

    activePerspective_ = std::move(saved);
    results.insert(results.end(), res.begin(), res.end());
  }
  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::resolveAnchor(const AnchorNode &anchor,
                         const std::vector<zigzag::CellRef> &contextCells) {
  std::vector<zigzag::CellRef> results;

  switch (anchor.kind) {
  case AnchorKind::Home:
    if (const auto h = coordinator_.homeAnchor()) {
      results.push_back(*h);
    }
    break;

  case AnchorKind::NamedStore: {
    if (const auto storeHome = coordinator_.resolveNamedStore(anchor.name)) {
      results.push_back(*storeHome);
    }
    break;
  }

  case AnchorKind::Root:
    if (const auto h = coordinator_.homeAnchor()) {
      results.push_back(*h);
    }
    break;

  case AnchorKind::Cursor:
    results.push_back(core_->dims().cursors);
    break;

  case AnchorKind::NamedCursor:
  case AnchorKind::AllCursors:
  case AnchorKind::AllNamedCursors:
    if (const auto h = coordinator_.homeAnchor()) {
      results.push_back(*h);
    }
    break;

  case AnchorKind::Variable: {
    if (const auto functor = functorNamed(anchor.name)) {
      // A functor in anchor position reads from the context, as `/...` does.
      auto from = contextCells;
      if (from.empty()) {
        if (auto it = env_.find("."); it != env_.end()) {
          from = it->second;
        }
      }
      results = applyFunctor(**functor, std::move(from));
      break;
    }
    auto it = env_.find(anchor.name);
    if (it != env_.end()) {
      results = it->second;
    }
    break;
  }

  case AnchorKind::LiteralCellId: {
    auto ref = static_cast<zigzag::CellRef>(anchor.cellId);
    if (core_->arena().contains(ref)) {
      results.push_back(ref);
    }
    break;
  }

  case AnchorKind::Context:
    if (!contextCells.empty()) {
      results = contextCells;
    } else {
      auto it = env_.find(".");
      if (it != env_.end()) {
        results = it->second;
      }
    }
    break;

  case AnchorKind::Create: {
    std::optional<zigzag::vortex::CellValue> value;
    if (anchor.createValue) {
      const auto &createValue = *anchor.createValue;
      if (createValue.kind == CreateValue::Kind::Literal) {
        value = createValue.literal;
      } else if (createValue.kind == CreateValue::Kind::Expression) {
        if (!createValue.expr) break;
        value = evaluateValueExpr(*createValue.expr, std::nullopt);
        if (!value) break;
      }
    }
    const zigzag::CellRef fresh = core_->arena().makeCell();
    if (value) core_->writeValue(fresh, 0, std::nullopt, *value);
    results.push_back(fresh);
    break;
  }
  case AnchorKind::StoreGenerator: {
    if (!anchor.generator) break;
    const auto &generator = *anchor.generator;
    if (generator.opName == "slices" || generator.opName == "docs") {
      std::optional<std::string> name;
      if (!generator.args.empty()) {
        const auto value =
            evaluateValueExpr(generator.args.front(), std::nullopt);
        if (!value) break;
        name = textOfValue(*value);
      }
      PathStep selector;
      if (generator.opName == "slices")
        selector.selector = SliceSelectorStep{.nameFilter = name};
      else
        selector.selector = DocSelectorStep{.nameFilter = name};
      results = evaluateStep(selector, {});
    } else if (generator.opName == "current" || generator.opName == "local" ||
               generator.opName == "system") {
      results = resolveStoreAnchor(
          StoreAnchor{.method = generator.opName, .args = generator.args});
    } else {
      throw std::runtime_error("store generator requires a connected discovery "
                               "provider: " +
                               generator.opName);
    }
    break;
  }
  case AnchorKind::StoreAnchor: {
    if (anchor.storeAnchor) {
      results = resolveStoreAnchor(*anchor.storeAnchor);
    }
    break;
  }
  case AnchorKind::Microversion: {
    if (const auto h = coordinator_.homeAnchor()) {
      results.push_back(*h);
      if (anchor.versionPin) {
        results = applyVersionPin(*anchor.versionPin, results);
      } else {
        VersionPin pin;
        VersionExpr version;
        version.versionStr = anchor.microversionId;
        pin.versions.push_back(std::move(version));
        results = applyVersionPin(pin, results);
      }
    }
    break;
  }
  }

  if (anchor.derefMaster) {
    for (auto &c : results) {
      this->arena().consumeWork();
      c = zigzag::queryCloneMaster(core_->arena(), c, core_->dims().clone)
              .value_or(c);
    }
  }

  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::evaluatePath(const PathExpression &path,
                        const std::vector<zigzag::CellRef> &contextCells) {
  QueryExpression query;
  query.expr = path;
  zigzag::vortex::VortexVM vm(*core_);
  QueryExecutor executor(*this, vm);
  return valueOrThrow(executor.execute(query, contextCells));
}

std::vector<zigzag::CellRef>
VQLEngine::traverseDimension(const SignedDimensionStep &dimStep,
                             const std::vector<zigzag::CellRef> &inputs) {
  std::vector<zigzag::CellRef> results;
  zigzag::DimRef dim    = resolveDimension(dimStep.dimName);
  zigzag::DimVector dir = dimStep.direction;

  auto &arena = core_->arena();
  // Walks through arena.linkedMinting(), so a step from a loaded store's cell
  // reaches the rest of that store; a cycle is walked once.
  const auto walk = [&](const zigzag::CellRef start,
                        const zigzag::DimVector way, const auto &visit) {
    std::unordered_set<zigzag::CellRef> seen{start};
    for (auto cell = arena.linkedMinting(start, dim, way);
         cell && seen.insert(*cell).second;
         cell = arena.linkedMinting(*cell, dim, way)) {
      this->arena().consumeWork();
      visit(*cell);
    }
  };
  const auto extreme = [&](const zigzag::CellRef start,
                           const zigzag::DimVector way) {
    auto last = start;
    walk(start, way, [&](const zigzag::CellRef cell) { last = cell; });
    return last;
  };

  for (zigzag::CellRef in : inputs) {
    this->arena().consumeWork();
    if (auto projected = traverseView(dimStep, in)) {
      results.insert(results.end(), projected->begin(), projected->end());
      continue;
    }
    if (!arena.contains(in)) {
      continue;
    }

    switch (dimStep.placement) {
    case Placement::Default:
    case Placement::From:
      // Walk outward from context in step's direction (or both if isBidir)
      if (dimStep.isBidir) {
        walk(in, zigzag::DimVector::POS,
             [&](const zigzag::CellRef cell) { results.push_back(cell); });
        walk(in, zigzag::DimVector::NEG,
             [&](const zigzag::CellRef cell) { results.push_back(cell); });
      } else {
        walk(in, dir,
             [&](const zigzag::CellRef cell) { results.push_back(cell); });
      }
      break;

    case Placement::Rank: {
      // Seek to head (extreme negward), then stream to tail (extreme posward)
      const auto head = extreme(in, zigzag::DimVector::NEG);
      std::vector<zigzag::CellRef> rankCells{head};
      walk(head, zigzag::DimVector::POS,
           [&](const zigzag::CellRef cell) { rankCells.push_back(cell); });
      if (dir == zigzag::DimVector::NEG) {
        std::ranges::reverse(rankCells);
      }
      results.insert(results.end(), rankCells.begin(), rankCells.end());
      break;
    }

    case Placement::Head:
      results.push_back(extreme(in, zigzag::DimVector::NEG));
      break;

    case Placement::Tail:
      results.push_back(extreme(in, zigzag::DimVector::POS));
      break;
    }
  }

  if (results.empty()) {
    auto mountIt = sessionMounts_.find(dimStep.dimName);
    if (mountIt != sessionMounts_.end()) {
      if (dimStep.isBidir || dimStep.direction == mountIt->second.direction) {
        results.insert(results.end(), mountIt->second.roots.begin(),
                       mountIt->second.roots.end());
      }
    }
  }

  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::performCreates(const SignedDimensionStep &dimStep,
                          const std::vector<zigzag::CellRef> &inputs) {
  if (dimStep.placement == Placement::From ||
      dimStep.placement == Placement::Rank) {
    throw std::runtime_error(
        "::from and ::rank have no meaning on a create (Rule §4.1)");
  }

  std::vector<zigzag::CellRef> results;
  zigzag::DimRef dim = resolveDimension(dimStep.dimName);

  for (zigzag::CellRef in : inputs) {
    this->arena().consumeWork();
    if (auto projected = traverseView(dimStep, in)) {
      results.insert(results.end(), projected->begin(), projected->end());
      continue;
    }
    if (!core_->arena().contains(in)) {
      continue;
    }

    // Default create seeks to tail; ::head inserts at head
    bool insertAtHead = (dimStep.placement == Placement::Head);
    // ::head attaches before the rank's head, anything else after its tail.
    zigzag::CellRef attachPoint =
        zigzag::rankTail(core_->arena(), in, dim,
                         insertAtHead ? zigzag::Negward : zigzag::Posward);

    std::vector<zigzag::CellRef> created;
    for (const auto &c : dimStep.creates) {
      this->arena().consumeWork();
      std::optional<zigzag::CellRef> newCell;
      if (c.kind == CreateValue::Kind::Empty) {
        newCell = core_->arena().makeCell();
      } else if (c.kind == CreateValue::Kind::Literal) {
        newCell = core_->arena().makeCell(c.literal);
      } else if (c.kind == CreateValue::Kind::Expression) {
        if (!c.expr) continue;
        if (const auto val = evaluateValueExpr(*c.expr, in)) {
          newCell = cellFromValue(*val);
        } else {
          continue;
        }
      }
      if (!newCell) continue;
      const auto createdCell = *newCell;

      if (insertAtHead) {
        zigzag::expectWritten(core_->arena().link(
            createdCell, dim, zigzag::DimVector::POS, attachPoint));
        zigzag::expectWritten(core_->arena().link(
            attachPoint, dim, zigzag::DimVector::NEG, createdCell));
      } else {
        zigzag::expectWritten(core_->arena().link(
            attachPoint, dim, zigzag::DimVector::POS, createdCell));
        zigzag::expectWritten(core_->arena().link(
            createdCell, dim, zigzag::DimVector::NEG, attachPoint));
      }
      attachPoint = createdCell;
      created.push_back(createdCell);
    }

    // Yield handling
    switch (dimStep.placement) {
    default:
      break;
    }

    // YieldMode on create
    // Handled by evaluateStep based on yieldMode
    results.insert(results.end(), created.begin(), created.end());
  }

  return results;
}

const StoreInfo *VQLEngine::storeForCell(const zigzag::CellRef cell) const {
  if (const auto source = core_->arena().resolveSourceCell(cell)) {
    const auto found =
        std::ranges::find_if(coordinator_.stores(), [&](const StoreInfo &info) {
          return info.store.get() == source->first;
        });
    if (found != coordinator_.stores().end()) return &*found;
  }
  const auto stores = coordinator_.stores();
  if (stores.empty()) return nullptr;
  const auto primary =
      std::ranges::find(stores, std::string_view("primary"), &StoreInfo::role);
  return primary == stores.end() ? &stores.front() : &*primary;
}

std::optional<xanadu::MicroversionId>
VQLEngine::evaluateVersionExpr(const VersionExpr &expr,
                               const std::optional<zigzag::CellRef> context) {
  using Op = VersionExpr::Op;
  if (expr.op == Op::Range) return std::nullopt;
  if (expr.op == Op::Literal) {
    try {
      return xanadu::MicroversionId::parse(expr.versionStr);
    } catch (const std::invalid_argument &) {
      return std::nullopt;
    }
  }
  if (expr.op == Op::Variable) {
    const auto found = env_.find(expr.versionStr);
    if (found == env_.end() || found->second.empty()) return std::nullopt;
    const auto rendered = core_->render(found->second.front());
    if (!rendered || !std::holds_alternative<std::string>(*rendered)) {
      return std::nullopt;
    }
    try {
      return xanadu::MicroversionId::parse(std::get<std::string>(*rendered));
    } catch (const std::invalid_argument &) {
      return std::nullopt;
    }
  }
  if (!expr.left) return std::nullopt;
  auto version = evaluateVersionExpr(*expr.left, context);
  if (!version) return std::nullopt;
  if (expr.op == Op::Next || expr.op == Op::Parent) {
    if (expr.stepOffset < 0) return std::nullopt;
    for (std::int64_t i = 0; i < expr.stepOffset; ++i) {
      this->arena().consumeWork();
      *version = expr.op == Op::Next ? version->next() : version->parent();
    }
    return version;
  }
  if (expr.op == Op::Branch) {
    std::uint64_t ordinal = 0;
    for (const char ch : expr.branchName) {
      if (ch < 'a' || ch > 'z') return std::nullopt;
      const auto digit = static_cast<std::uint64_t>(ch - 'a' + 1);
      if (ordinal > (UINT32_MAX - digit) / 26) return std::nullopt;
      ordinal = ordinal * 26 + digit;
    }
    if (ordinal == 0) return std::nullopt;
    return version->branch(static_cast<std::uint32_t>(ordinal));
  }
  return std::nullopt;
}

std::vector<zigzag::CellRef>
VQLEngine::applyVersionPin(const VersionPin &pin,
                           const std::vector<zigzag::CellRef> &cells) {
  if (pin.versions.empty()) return {};
  std::vector<zigzag::CellRef> pinned;
  for (const auto input : cells) {
    this->arena().consumeWork();
    const auto *info = storeForCell(input);
    if (!info || !info->store) continue;
    std::vector<xanadu::MicroversionId> versions;
    if (pin.isRange && pin.versions.size() >= 2) {
      const auto first = evaluateVersionExpr(pin.versions[0], input);
      const auto last  = evaluateVersionExpr(pin.versions[1], input);
      if (!first || !last || !first->isAncestorOf(*last)) continue;
      for (const auto &version : last->path()) {
        this->arena().consumeWork();
        if (*first <= version && version <= *last) versions.push_back(version);
      }
    } else {
      for (const auto &expr : pin.versions) {
        this->arena().consumeWork();
        if (const auto version = evaluateVersionExpr(expr, input)) {
          versions.push_back(*version);
        }
      }
    }
    const auto available = info->store->allVersions();
    for (const auto &version : versions) {
      this->arena().consumeWork();
      if (std::ranges::find(available, version) == available.end()) continue;
      const auto cacheKey =
          std::to_string(reinterpret_cast<std::uintptr_t>(info->store.get())) +
          ":" + version.str();
      const auto label = info->label + "@" + version.str();
      auto space       = historicalSpaces_.find(cacheKey);
      std::optional<std::uint32_t> known;
      // A release() can detach a cached space and reuse its id for another,
      // so an id is trusted only while it still names this store's fold.
      if (space != historicalSpaces_.end()) {
        const auto cached = core_->arena().spaceAt(space->second);
        if (cached && cached->store == info->store.get() &&
            cached->label == label)
          known = space->second;
        else
          historicalSpaces_.erase(space);
      }
      if (!known) {
        auto fold = std::make_shared<zigzag::Manifold>(
            info->store->rebuildManifold(version));
        if (!fold->home()) continue;
        known = core_->arena().attach(zigzag::Space{
            .manifold      = fold.get(),
            .ownedManifold = fold,
            .store         = info->store.get(),
            .reader        = info->store.get(),
            .label         = label,
        });
        core_->arena().bindDimensionsByNameMatch();
        historicalSpaces_.emplace(cacheKey, *known);
      }
      const auto spaceId  = *known;
      const auto attached = core_->arena().spaceAt(spaceId);
      if (!attached || !attached->manifold) continue;
      if (const auto source = core_->arena().resolveSourceCell(input)) {
        if (source->first == info->store.get()) {
          if (attached->manifold->contains(source->second)) {
            pinned.push_back(core_->arena().proxyFor(spaceId, source->second));
          }
        }
      } else if (info->homeCell == input) {
        if (const auto home = attached->manifold->home()) {
          pinned.push_back(core_->arena().proxyFor(spaceId, *home));
        }
      }
    }
  }
  return pinned;
}

std::vector<zigzag::CellRef>
VQLEngine::evaluateStep(const PathStep &step,
                        const std::vector<zigzag::CellRef> &inputCells) {
  PathExpression path;
  path.anchor.kind = AnchorKind::Context;
  path.steps.push_back(step);
  return evaluatePath(path, inputCells);
}

zigzag::CellRef VQLEngine::cellFromValue(const zigzag::vortex::CellValue &val) {
  if (std::holds_alternative<double>(val)) {
    return core_->arena().makeScalarCell(std::get<double>(val));
  }
  if (std::holds_alternative<std::int64_t>(val)) {
    return core_->arena().makeScalarCell(std::get<std::int64_t>(val));
  }
  if (std::holds_alternative<bool>(val)) {
    return core_->arena().makeScalarCell(std::get<bool>(val));
  }
  return core_->arena().makeCell(std::get<std::string>(val));
}

std::optional<zigzag::CellRef>
VQLEngine::deltaTarget(const FunctionInvocation &fn,
                       std::optional<zigzag::CellRef> context) {
  if (fn.args.empty()) return context;
  if (const auto *variable = std::get_if<std::string>(&fn.args.front().kind)) {
    const auto it = env_.find(*variable);
    if (it == env_.end() || it->second.empty()) return std::nullopt;
    return it->second.front();
  }
  if (const auto *path =
          std::get_if<std::shared_ptr<PathExpression>>(&fn.args.front().kind)) {
    const auto cells =
        evaluatePath(**path, context ? std::vector<zigzag::CellRef>{*context}
                                     : std::vector<zigzag::CellRef>{});
    if (cells.empty()) return std::nullopt;
    return cells.front();
  }
  return std::nullopt;
}

std::optional<zigzag::vortex::CellValue>
VQLEngine::evaluateValueExpr(const ValueExpr &expr,
                             std::optional<zigzag::CellRef> context) {
  this->arena().consumeWork();
  if (std::holds_alternative<ScalarLiteral>(expr.kind)) {
    const auto &lit = std::get<ScalarLiteral>(expr.kind);
    if (std::holds_alternative<std::string>(lit.value)) {
      return zigzag::vortex::CellValue{std::get<std::string>(lit.value)};
    }
    if (std::holds_alternative<double>(lit.value)) {
      return zigzag::vortex::CellValue{std::get<double>(lit.value)};
    }
    if (std::holds_alternative<std::int64_t>(lit.value)) {
      return zigzag::vortex::CellValue{std::get<std::int64_t>(lit.value)};
    }
    if (std::holds_alternative<bool>(lit.value)) {
      return zigzag::vortex::CellValue{std::get<bool>(lit.value)};
    }
  }

  if (std::holds_alternative<std::string>(expr.kind)) {
    const auto &varName = std::get<std::string>(expr.kind);
    if (functorNamed(varName)) {
      const auto reached = cellsOfValueExpr(expr, context);
      if (reached.empty()) return std::nullopt;
      return core_->render(reached.front());
    }
    auto it = env_.find(varName);
    if (it != env_.end() && !it->second.empty()) {
      return core_->render(it->second[0]);
    }
    return std::nullopt;
  }

  if (std::holds_alternative<std::shared_ptr<PathExpression>>(expr.kind)) {
    const auto &path = *std::get<std::shared_ptr<PathExpression>>(expr.kind);
    auto cells =
        evaluatePath(path, context ? std::vector<zigzag::CellRef>{*context}
                                   : std::vector<zigzag::CellRef>{});
    if (!cells.empty()) {
      return core_->render(cells[0]);
    }
    return std::nullopt;
  }

  if (std::holds_alternative<std::shared_ptr<FunctionInvocation>>(expr.kind)) {
    const auto &fn = *std::get<std::shared_ptr<FunctionInvocation>>(expr.kind);
    if (fn.name == "contains" && fn.args.size() >= 2) {
      const auto haystack = evaluateValueExpr(fn.args[0], context);
      const auto needle   = evaluateValueExpr(fn.args[1], context);
      if (!haystack || !needle) return zigzag::vortex::CellValue{false};
      return zigzag::vortex::CellValue{
          std::string::npos !=
          textOfValue(*haystack).find(textOfValue(*needle))};
    }
    if (fn.name == "count" && !fn.args.empty()) {
      if (const auto *path =
              std::get_if<std::shared_ptr<PathExpression>>(&fn.args[0].kind)) {
        const auto cells = evaluatePath(
            **path, context ? std::vector<zigzag::CellRef>{*context}
                            : std::vector<zigzag::CellRef>{});
        return zigzag::vortex::CellValue{
            static_cast<std::int64_t>(cells.size())};
      }
    }
    if (fn.name == "delta") {
      const auto target = deltaTarget(fn, context);
      if (!fn.args.empty() && !target) return std::nullopt;
      if (target && core_->arena().contains(*target)) {
        const auto dDelta = resolveDimension("d.delta");
        if (const auto deltaCell = core_->arena().linkedMinting(
                *target, dDelta, zigzag::DimVector::POS)) {
          return zigzag::vortex::CellValue{core_->arena().textOf(*deltaCell)};
        }
      }
      return zigzag::vortex::CellValue{std::string("modified")};
    }

    if (const auto itDef = userDefs_.find(fn.name); itDef != userDefs_.end()) {
      requirePositionalArguments(fn);
      const auto results = callUserDef(itDef->second, fn.args, context);
      if (results.empty()) return std::nullopt;
      return core_->render(results.front());
    }
  }

  if (std::holds_alternative<std::shared_ptr<PathLambda>>(expr.kind)) {
    const auto &lambda = *std::get<std::shared_ptr<PathLambda>>(expr.kind);
    const auto results = executeLambda(lambda, context);
    if (results.empty()) return std::nullopt;
    return core_->render(results.front());
  }

  if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(expr.kind)) {
    return std::nullopt;
  }

  if (std::holds_alternative<std::shared_ptr<VersionExpr>>(expr.kind)) {
    const auto &ve     = *std::get<std::shared_ptr<VersionExpr>>(expr.kind);
    const auto version = evaluateVersionExpr(ve, context);
    if (!version) return std::nullopt;
    return zigzag::vortex::CellValue{version->str()};
  }

  if (std::holds_alternative<std::shared_ptr<BooleanExpr>>(expr.kind)) {
    const auto &b = *std::get<std::shared_ptr<BooleanExpr>>(expr.kind);
    return zigzag::vortex::CellValue{evaluatePredicate(b, context)};
  }

  if (std::holds_alternative<std::shared_ptr<BinaryValueExpr>>(expr.kind))
    throw QueryRefusal(
        QueryErrorCategory::Unsupported,
        "binary value expressions require Vortex VM graph lowering");

  return std::nullopt;
}

bool VQLEngine::compareValues(const zigzag::vortex::CellValue &left, CompOp op,
                              const zigzag::vortex::CellValue &right) {
  // Numeric comparison check
  bool leftNum  = false;
  bool rightNum = false;
  double lD     = cellValueToDouble(left, leftNum);
  double rD     = cellValueToDouble(right, rightNum);

  if (leftNum && rightNum) {
    switch (op) {
    case CompOp::Equal:
      return std::abs(lD - rD) < 1e-9;
    case CompOp::NotEqual:
      return std::abs(lD - rD) >= 1e-9;
    case CompOp::LessThan:
      return lD < rD;
    case CompOp::GreaterThan:
      return lD > rD;
    case CompOp::LessEqual:
      return lD <= rD;
    case CompOp::GreaterEqual:
      return lD >= rD;
    }
  }

  // String comparison fallback
  std::string lStr;
  if (std::holds_alternative<std::string>(left)) {
    lStr = std::get<std::string>(left);
  } else if (leftNum) {
    lStr = std::to_string(lD);
  }
  std::string rStr;
  if (std::holds_alternative<std::string>(right)) {
    rStr = std::get<std::string>(right);
  } else if (rightNum) {
    rStr = std::to_string(rD);
  }

  switch (op) {
  case CompOp::Equal:
    return lStr == rStr;
  case CompOp::NotEqual:
    return lStr != rStr;
  case CompOp::LessThan:
    return lStr < rStr;
  case CompOp::GreaterThan:
    return lStr > rStr;
  case CompOp::LessEqual:
    return lStr <= rStr;
  case CompOp::GreaterEqual:
    return lStr >= rStr;
  }
  return false;
}

bool VQLEngine::evaluateFactor(const BooleanFactor &factor,
                               std::optional<zigzag::CellRef> context) {
  this->arena().consumeWork();
  bool result = false;

  if (std::holds_alternative<ComparisonExpr>(factor.test)) {
    const auto &cmp     = std::get<ComparisonExpr>(factor.test);
    const auto leftVal  = evaluateValueExpr(*cmp.left, context);
    const auto rightVal = evaluateValueExpr(*cmp.right, context);
    const auto lhs      = versionValue(*cmp.left, context);
    const auto rhs      = versionValue(*cmp.right, context);
    if (lhs && rhs) {
      if (lhs && rhs) {
        const bool ancestor        = lhs->isAncestorOf(*rhs);
        const bool reverseAncestor = rhs->isAncestorOf(*lhs);
        switch (cmp.op) {
        case CompOp::Equal:
          result = *lhs == *rhs;
          break;
        case CompOp::NotEqual:
          result = *lhs != *rhs;
          break;
        case CompOp::LessThan:
          result = ancestor && *lhs != *rhs;
          break;
        case CompOp::GreaterThan:
          result = reverseAncestor && *lhs != *rhs;
          break;
        case CompOp::LessEqual:
          result = ancestor;
          break;
        case CompOp::GreaterEqual:
          result = reverseAncestor;
          break;
        }
      }
    } else {
      result =
          leftVal && rightVal && compareValues(*leftVal, cmp.op, *rightVal);
    }
  } else {
    const auto &test = std::get<PredicateTest>(factor.test);
    if (std::holds_alternative<std::shared_ptr<PathExpression>>(test.kind)) {
      const auto &path = *std::get<std::shared_ptr<PathExpression>>(test.kind);
      auto cells =
          evaluatePath(path, context ? std::vector<zigzag::CellRef>{*context}
                                     : std::vector<zigzag::CellRef>{});
      result = !cells.empty();
    } else if (std::holds_alternative<bool>(test.kind)) {
      result = context ? core_->isTruthy(*context).value_or(false) : false;
    } else if (std::holds_alternative<std::shared_ptr<ValueExpr>>(test.kind)) {
      if (const auto val = evaluateValueExpr(
              *std::get<std::shared_ptr<ValueExpr>>(test.kind), context)) {
        result = zigzag::vortex::VortexCore::evaluateTruthiness(*val);
      }
    } else if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(
                   test.kind)) {
      result = evaluateShapePredicate(
          *std::get<std::shared_ptr<ShapeLiteral>>(test.kind), context);
    } else if (const auto *function =
                   std::get_if<std::shared_ptr<FunctionInvocation>>(&test.kind);
               function && *function) {
      const ValueExpr expression{.kind = *function};
      if (const auto value = evaluateValueExpr(expression, context))
        result = zigzag::vortex::VortexCore::evaluateTruthiness(*value);
    }
  }

  return factor.negated ? !result : result;
}

bool VQLEngine::evaluateTerm(const BooleanTerm &term,
                             std::optional<zigzag::CellRef> context) {
  this->arena().consumeWork();
  return std::ranges::all_of(term.factors, [this, context](const auto &factor) {
    return evaluateFactor(factor, context);
  }); // Short-circuit AND
}

bool VQLEngine::evaluatePredicate(const BooleanExpr &expr,
                                  std::optional<zigzag::CellRef> context) {
  this->arena().consumeWork();
  return std::ranges::any_of(expr.terms, [this, context](const auto &term) {
    return evaluateTerm(term, context);
  }); // Short-circuit OR
}

std::vector<zigzag::CellRef>
VQLEngine::executeBlock(const ExecutionBlock &block) {
  std::vector<zigzag::CellRef> results;
  evaluateBindings(block, 0, results);
  return results;
}

void VQLEngine::evaluateBindings(
    const ExecutionBlock &block, std::size_t bindingIndex,
    std::vector<zigzag::CellRef> &accumulatedResults) {
  this->arena().consumeWork();
  if (bindingIndex >= block.bindings.size()) {
    // All bindings completed, test where clause
    if (block.where.has_value()) {
      if (!evaluatePredicate(block.where->condition, std::nullopt)) {
        return;
      }
    }

    executeActionClause(block.action, accumulatedResults);
    return;
  }

  const auto &binding = block.bindings[bindingIndex];
  if (std::holds_alternative<ForClause>(binding)) {
    const auto &fc = std::get<ForClause>(binding);
    auto inCells   = evaluatePath(fc.inPath);
    for (zigzag::CellRef c : inCells) {
      this->arena().consumeWork();
      if (!fc.varName.empty()) {
        bindName(fc.varName, {c});
      }
      if (fc.destructurePattern) {
        // A missing field binds nothing, but a written type still refuses.
        Bindings scoped;
        if (!matchDestructure(*fc.destructurePattern, c, scoped,
                              MissingField::BindEmpty)) {
          continue;
        }
        for (auto &[name, bound] : scoped) {
          bindName(name, std::move(bound));
        }
      }
      evaluateBindings(block, bindingIndex + 1, accumulatedResults);
    }
  } else {
    bindLet(std::get<LetClause>(binding));
    evaluateBindings(block, bindingIndex + 1, accumulatedResults);
  }
}

void VQLEngine::executeActionClause(
    const ActionClause &action,
    std::vector<zigzag::CellRef> &accumulatedResults) {
  if (std::holds_alternative<ReturnClause>(action.clause)) {
    const auto &ret = std::get<ReturnClause>(action.clause);
    for (const auto &item : ret.items) {
      this->arena().consumeWork();
      if (std::holds_alternative<PathExpression>(item.item)) {
        auto res = evaluatePath(std::get<PathExpression>(item.item));
        accumulatedResults.insert(accumulatedResults.end(), res.begin(),
                                  res.end());
      } else if (std::holds_alternative<FieldWeave>(item.item)) {
        const auto &fw             = std::get<FieldWeave>(item.item);
        zigzag::CellRef resultCell = core_->arena().makeCell();
        std::vector<zigzag::CellRef> cur{resultCell};
        for (const auto &step : fw.steps) {
          this->arena().consumeWork();
          cur = evaluateStep(step, cur);
        }
        accumulatedResults.push_back(resultCell);
      } else if (std::holds_alternative<ReturnValue>(item.item)) {
        throw QueryRefusal(
            QueryErrorCategory::Unsupported,
            "value return expressions require Vortex graph lowering");
      }
    }
  } else if (std::holds_alternative<EffectClause>(action.clause)) {
    executeEffectClause(std::get<EffectClause>(action.clause),
                        accumulatedResults);
  } else if (std::holds_alternative<ConditionalClause>(action.clause)) {
    const auto &cond = std::get<ConditionalClause>(action.clause);
    if (evaluatePredicate(cond.condition, std::nullopt)) {
      if (cond.thenClause) {
        executeActionClause(*cond.thenClause, accumulatedResults);
      }
    } else {
      if (cond.elseClause) {
        executeActionClause(*cond.elseClause, accumulatedResults);
      }
    }
  }
}

void VQLEngine::executeEffectClause(
    const EffectClause &eff, std::vector<zigzag::CellRef> &accumulatedResults) {
  for (const auto &item : eff.items) {
    this->arena().consumeWork();
    if (std::holds_alternative<PathExpression>(item.item)) {
      auto res = evaluatePath(std::get<PathExpression>(item.item));
      accumulatedResults.insert(accumulatedResults.end(), res.begin(),
                                res.end());
    } else if (std::holds_alternative<LetClause>(item.item)) {
      const auto bound = bindLet(std::get<LetClause>(item.item));
      accumulatedResults.insert(accumulatedResults.end(), bound.begin(),
                                bound.end());
    } else {
      const auto &p =
          std::get<std::pair<ForClause, std::shared_ptr<EffectClause>>>(
              item.item);
      auto inCells = evaluatePath(p.first.inPath);
      for (zigzag::CellRef c : inCells) {
        this->arena().consumeWork();
        bindName(p.first.varName, {c});
        if (p.second) {
          executeEffectClause(*p.second, accumulatedResults);
        }
      }
    }
  }
}

} // namespace xanadu::vql

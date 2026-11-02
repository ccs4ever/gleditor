/**
 * @file vql_engine.cpp
 * @brief Direct query evaluation engine for VQL v16.0.
 */
#include "common/xanadu/vql/vql_engine.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <concepts>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include <gleditor/cpp26_concat.hpp>

namespace xanadu::vql {
using zigzag::DimVector;

namespace {

class PerspectiveScope {
public:
  PerspectiveScope(std::string &perspective, std::string selected)
      : perspective_(perspective), previous_(std::move(perspective)) {
    perspective_ = std::move(selected);
  }
  PerspectiveScope(const PerspectiveScope &)            = delete;
  PerspectiveScope &operator=(const PerspectiveScope &) = delete;
  ~PerspectiveScope() { perspective_.swap(previous_); }

private:
  std::string &perspective_;
  std::string previous_;
};

template <class Bindings, class Functors> class BindingScope {
public:
  BindingScope(Bindings &bindings, Functors &functors)
      : bindings_(bindings), functors_(functors), savedBindings_(bindings),
        savedFunctors_(functors) {}
  BindingScope(const BindingScope &)            = delete;
  BindingScope &operator=(const BindingScope &) = delete;
  ~BindingScope() {
    bindings_.swap(savedBindings_);
    functors_.swap(savedFunctors_);
  }

private:
  Bindings &bindings_;
  Functors &functors_;
  Bindings savedBindings_;
  Functors savedFunctors_;
};

std::vector<zigzag::CellRef>
concatCellStreams(const std::span<const zigzag::CellRef> current,
                  const std::span<const zigzag::CellRef> created) {
  std::vector<zigzag::CellRef> combined;
  combined.reserve(current.size() + created.size());
  auto cells = gleditor::cpp26::views::concat(current, created);
  std::ranges::copy(cells, std::back_inserter(combined));
  return combined;
}

bool stringToDouble(std::string_view sv, double &out) {
  if (sv.empty()) {
    return false;
  }
  std::string s(sv);
  char *end = nullptr;
  out       = std::strtod(s.c_str(), &end);
  return end == s.c_str() + s.size();
}

bool stringToInt64(std::string_view sv, std::int64_t &out) {
  if (sv.empty()) {
    return false;
  }
  const char *begin = sv.data();
  const char *end   = sv.data() + sv.size();
  auto [ptr, ec]    = std::from_chars(begin, end, out);
  return ec == std::errc{} && ptr == end;
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
    : coordinator_(coordinator), core_(&coordinator.core()) {}

VQLEngine::VQLEngine(zigzag::vortex::VortexCore &core)
    : ownedCoordinator_(std::make_unique<MultiStoreCoordinator>(core)),
      coordinator_(*ownedCoordinator_), core_(&core) {}

VQLEngine::VQLEngine(zigzag::ArenaManifold &arena)
    : ownedCore_(std::make_unique<zigzag::vortex::VortexCore>(arena)),
      ownedCoordinator_(std::make_unique<MultiStoreCoordinator>(*ownedCore_)),
      coordinator_(*ownedCoordinator_), core_(ownedCore_.get()) {}

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
}

std::vector<zigzag::CellRef> VQLEngine::execute(std::string_view queryString) {
  Parser parser(queryString);
  QueryExpression query = parser.parseQuery();
  return execute(query);
}

std::vector<zigzag::CellRef> VQLEngine::execute(const QueryExpression &query) {
  this->arena().consumeWork();

  for (const auto &def : query.defs) {
    userDefs_[def.name] = def;
  }

  for (const auto &mount : query.mounts) {
    executeMount(mount);
  }

  if (!query.body.statements.empty()) {
    return executeScriptBody(query.body);
  }

  if (std::holds_alternative<PathExpression>(query.expr)) {
    return evaluatePath(std::get<PathExpression>(query.expr));
  }
  return executeBlock(std::get<ExecutionBlock>(query.expr));
}

std::vector<zigzag::CellRef>
VQLEngine::executeScriptBody(const ScriptBody &body) {
  std::vector<zigzag::CellRef> lastResult;
  for (const auto &stmt : body.statements) {
    this->arena().consumeWork();
    lastResult = executeStatement(stmt);
  }
  return lastResult;
}

bool VQLEngine::matchDestructure(const DestructurePattern &pattern,
                                 std::optional<zigzag::CellRef> context,
                                 Bindings &scopedBindings,
                                 const MissingField missing) {
  if (!context || !core_->arena().contains(*context)) {
    return false;
  }
  for (const auto &field : pattern.fields) {
    if (field.isContext) {
      scopedBindings["."] = {*context};
      if (!field.varName.empty()) {
        scopedBindings[field.varName] = {*context};
      }
      continue;
    }
    zigzag::DimRef dim = resolveDimension(field.dimName);
    auto linked = core_->arena().linkedMinting(*context, dim, field.direction);
    if (field.isBidir && !linked) {
      linked = core_->arena().linkedMinting(
          *context, dim,
          (field.direction == zigzag::DimVector::POS) ? zigzag::DimVector::NEG
                                                      : zigzag::DimVector::POS);
    }
    std::vector<zigzag::CellRef> bound;
    if (linked) {
      bound.push_back(*linked);
    } else if (field.defaultValue) {
      bound = cellsOfValueExpr(*field.defaultValue, context);
    }
    // A typed field decides absence by its type (`T?` admits it); an untyped
    // one by the caller's rule.
    if (field.type) {
      if (!satisfiesType(*field.type, bound, &scopedBindings)) {
        return false;
      }
    } else if (bound.empty() && missing == MissingField::Refuse) {
      return false;
    }
    if (!field.varName.empty()) {
      scopedBindings[field.varName] = std::move(bound);
    }
  }
  return true;
}

bool VQLEngine::satisfiesType(const TypeExpr &type,
                              const std::span<const zigzag::CellRef> cells,
                              Bindings *const scopedBindings) {
  for (const auto &member : type.unionType.members) {
    // A member that fails partway must leave no bindings behind.
    Bindings trial;
    if (satisfiesPrimary(member, cells,
                         scopedBindings != nullptr ? &trial : nullptr)) {
      if (scopedBindings != nullptr) {
        for (auto &[name, bound] : trial) {
          (*scopedBindings)[name] = std::move(bound);
        }
      }
      return true;
    }
  }
  return false;
}

bool VQLEngine::satisfiesPrimary(const PrimaryType &type,
                                 const std::span<const zigzag::CellRef> cells,
                                 Bindings *const scopedBindings) {
  const auto *const generic = std::get_if<GenericType>(&type.kind);
  if (generic != nullptr) {
    if (generic->name != "Stream" || generic->args.size() != 1) {
      throw TypeContractError("unknown type " + generic->name + " with " +
                              std::to_string(generic->args.size()) +
                              " argument(s)");
    }
    // Each member is checked alone, so a shape inside a stream binds nothing:
    // one name cannot hold a field of every member at once.
    return std::ranges::all_of(cells, [&](const zigzag::CellRef cell) {
      return satisfiesType(*generic->args.front(), std::span{&cell, 1},
                           nullptr) &&
             (!type.refinement || [&] {
               Bindings ignored;
               return matchDestructure(type.refinement->pattern, cell, ignored);
             }());
    });
  }
  if (cells.empty()) {
    return type.isOptional;
  }
  // A non-stream type is one value; a stream of several is not one.
  return cells.size() == 1 &&
         satisfiesAtom(type, cells.front(), scopedBindings);
}

bool VQLEngine::satisfiesAtom(const PrimaryType &type,
                              const zigzag::CellRef cell,
                              Bindings *const scopedBindings) {
  Bindings ignored;
  Bindings &bindings   = scopedBindings != nullptr ? *scopedBindings : ignored;
  const bool inhabited = std::visit(
      [&](const auto &held) -> bool {
        using T = std::remove_cvref_t<decltype(held)>;
        if constexpr (std::same_as<T, BasicType>) {
          return hasBasicType(held, cell);
        } else if constexpr (std::same_as<T, ShapeType>) {
          return core_->arena().contains(cell) &&
                 matchDestructure(held.pattern, cell, bindings);
        } else if constexpr (std::same_as<T, std::shared_ptr<TypeExpr>>) {
          return satisfiesType(*held, std::span{&cell, 1}, scopedBindings);
        } else {
          return false; // GenericType is answered by satisfiesPrimary
        }
      },
      type.kind);
  return inhabited &&
         (!type.refinement ||
          matchDestructure(type.refinement->pattern, cell, bindings));
}

bool VQLEngine::hasBasicType(const BasicType &type,
                             const zigzag::CellRef cell) {
  using K     = BasicType::Kind;
  auto &arena = core_->arena();
  if (type.kind == K::Contextual) {
    throw TypeContractError("unknown type " + type.name);
  }
  if (!arena.contains(cell)) {
    return false;
  }
  // Scalars are read as comparisons read them (§4.3): a text cell whose whole
  // content is a number is that number, since imported and typed content is
  // text and only cells minted from literals carry value bits.
  const auto value = core_->render(cell);
  if (!value) return false;
  const auto &rendered   = *value;
  const auto *const text = std::get_if<std::string>(&rendered);
  switch (type.kind) {
  case K::Any:
  case K::Cell:
    return true;
  case K::Dim:
    // A front end's dimensions live on the core's d.dims rank, a store's in
    // the arena's own table; a cell is a Dim if either names it.
    return std::ranges::contains(arena.dimensions(), cell) ||
           (text != nullptr && core_->findDimension(*text) == cell);
  case K::Store:
    return coordinator_.coordinatorHome() == cell ||
           std::ranges::any_of(coordinator_.stores(), [&](const auto &info) {
             return info.storeCell == cell;
           });
  case K::Slice:
    return arena.home() == cell ||
           std::ranges::any_of(coordinator_.stores(), [&](const auto &info) {
             return info.homeCell == cell;
           });
  case K::Version:
    if (text == nullptr || text->empty()) {
      return false;
    }
    try {
      (void)xanadu::MicroversionId::parse(*text);
      return true;
    } catch (const std::invalid_argument &) {
      return false;
    }
  case K::Int: {
    std::int64_t parsed = 0;
    return std::holds_alternative<std::int64_t>(rendered) ||
           (text != nullptr && stringToInt64(*text, parsed));
  }
  case K::Float: {
    double parsed = 0.0;
    return std::holds_alternative<double>(rendered) ||
           std::holds_alternative<std::int64_t>(rendered) ||
           (text != nullptr && stringToDouble(*text, parsed));
  }
  case K::String:
    return text != nullptr;
  case K::Bool:
    return std::holds_alternative<bool>(rendered) ||
           (text != nullptr && (*text == "true" || *text == "false"));
  case K::Contextual:
    break;
  }
  return false;
}

std::vector<zigzag::CellRef>
VQLEngine::cellsOfValueExpr(const ValueExpr &expr,
                            const std::optional<zigzag::CellRef> context) {
  std::vector<zigzag::CellRef> contextCells;
  if (context) contextCells.push_back(*context);
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
  if (const auto *fn =
          std::get_if<std::shared_ptr<FunctionInvocation>>(&expr.kind)) {
    return evaluateStep(PathStep{.selector = **fn}, contextCells);
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
  if (!path.anchorPredicates.empty() || path.shapeProjection ||
      path.cloneTail) {
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
  // Inlining a bare `/$f` now binds the functor it names today, so a later
  // rebinding of $f does not reach into this one.
  for (const auto &step : path.steps) {
    const auto *const dimStep =
        std::get_if<SignedDimensionStep>(&step.selector);
    const bool bare = dimStep != nullptr && dimStep->dimName.starts_with('$') &&
                      dimStep->creates.empty() &&
                      dimStep->placement == Placement::Default &&
                      !step.derefMaster && step.predicates.empty() &&
                      step.shapePredicates.empty() && !step.rangeClamp &&
                      !step.versionPin && step.yieldMode == YieldMode::Default;
    const auto inner =
        bare ? functorNamed(std::string_view(dimStep->dimName).substr(1))
             : std::nullopt;
    if (inner) {
      steps.insert(steps.end(), (*inner)->begin(), (*inner)->end());
    } else {
      steps.push_back(step);
    }
  }
  return std::make_shared<const std::vector<PathStep>>(std::move(steps));
}

std::vector<zigzag::CellRef>
VQLEngine::applyFunctor(const std::vector<PathStep> &steps,
                        std::vector<zigzag::CellRef> cells) {
  for (const auto &step : steps) {
    this->arena().consumeWork();
    if (cells.empty()) {
      break;
    }
    cells = evaluateStep(step, cells);
  }
  return cells;
}

std::vector<zigzag::CellRef> VQLEngine::bindLet(const LetClause &let) {
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
  env_[name] = std::move(cells);
}

std::vector<zigzag::CellRef>
VQLEngine::executeLambda(const PathLambda &lambda,
                         std::optional<zigzag::CellRef> context) {
  this->arena().consumeWork();
  BindingScope scope(env_, functors_);
  Bindings scoped;

  if (!lambda.params.empty()) {
    const auto &param = lambda.params[0];
    if (std::holds_alternative<DestructurePattern>(param.target)) {
      if (!matchDestructure(std::get<DestructurePattern>(param.target), context,
                            scoped)) {
        return {};
      }
    } else {
      const auto given = context
                             ? std::span<const zigzag::CellRef>{&*context, 1}
                             : std::span<const zigzag::CellRef>{};
      if (param.type && !satisfiesType(*param.type, given, &scoped)) {
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

  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::callUserDef(const DefDeclaration &def,
                       const std::vector<ValueExpr> &args,
                       std::optional<zigzag::CellRef> context) {
  this->arena().consumeWork();
  const auto variadic = std::ranges::find_if(
      def.params, [](const LambdaParam &param) { return param.isVariadic; });
  if (variadic != def.params.end() && std::next(variadic) != def.params.end()) {
    throw std::runtime_error(def.name +
                             "() requires a final variadic parameter");
  }
  if (variadic == def.params.end() && args.size() > def.params.size()) {
    throw std::runtime_error(
        def.name + "() accepts at most " + std::to_string(def.params.size()) +
        " argument(s); got " + std::to_string(args.size()));
  }
  BindingScope scope(env_, functors_);

  struct Argument {
    std::vector<zigzag::CellRef> cells;
    std::optional<FunctorSteps> functor;
  };
  std::vector<Argument> arguments;
  arguments.reserve(args.size());
  // Explicit arguments belong to the caller. Installing any parameter first
  // could shadow a name used by a later argument or its functor capture.
  for (std::size_t index = 0; index < args.size(); ++index) {
    const auto &param = def.params[std::min(index, def.params.size() - 1)];
    const auto &arg   = args[index];
    Argument captured;
    if (!param.type && !param.isVariadic &&
        std::holds_alternative<std::string>(param.target)) {
      if (const auto *path =
              std::get_if<std::shared_ptr<PathExpression>>(&arg.kind)) {
        captured.functor = functorOf(**path);
      } else if (const auto *name = std::get_if<std::string>(&arg.kind)) {
        captured.functor = functorNamed(*name);
      }
    }
    if (!captured.functor) {
      captured.cells = cellsOfValueExpr(arg, context);
    }
    arguments.push_back(std::move(captured));
  }

  std::size_t argIdx = 0;
  for (std::size_t i = 0; i < def.params.size(); ++i) {
    const auto &param = def.params[i];
    if (param.isVariadic) {
      std::vector<zigzag::CellRef> restCells;
      while (argIdx < args.size()) {
        auto cells = std::move(arguments[argIdx++].cells);
        // A variadic's type is each argument's, not the whole tail's.
        if (param.type && !satisfiesType(*param.type, cells, nullptr)) {
          return {};
        }
        restCells.insert(restCells.end(), cells.begin(), cells.end());
      }
      if (std::holds_alternative<std::string>(param.target)) {
        bindName(std::get<std::string>(param.target), std::move(restCells));
      }
      break;
    }

    Argument *arg = argIdx < arguments.size() ? &arguments[argIdx++] : nullptr;
    if (arg != nullptr && arg->functor) {
      const auto &name = std::get<std::string>(param.target);
      env_.erase(name);
      functors_[name] = std::move(*arg->functor);
      continue;
    }

    std::vector<zigzag::CellRef> paramCells;
    if (arg != nullptr) {
      paramCells = std::move(arg->cells);
    } else if (param.defaultValue) {
      // Defaults intentionally see earlier callee parameters, unlike explicit
      // arguments, which were all captured in the caller's environment.
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
        return {};
      }
    } else {
      if (param.type && !satisfiesType(*param.type, paramCells, &scoped)) {
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
  this->arena().consumeWork();
  if (std::holds_alternative<ExecutionBlock>(stmt)) {
    return executeBlock(std::get<ExecutionBlock>(stmt));
  }
  if (std::holds_alternative<PathExpression>(stmt)) {
    return evaluatePath(std::get<PathExpression>(stmt));
  }
  if (std::holds_alternative<WeaveStatement>(stmt)) {
    std::vector<zigzag::CellRef> results;
    executeEffectClause(std::get<WeaveStatement>(stmt).effect, results);
    return results;
  }
  if (std::holds_alternative<WatchStatement>(stmt)) {
    return executeWatch(std::get<WatchStatement>(stmt));
  }
  if (std::holds_alternative<PerspectiveBlock>(stmt)) {
    return executePerspectiveBlock(std::get<PerspectiveBlock>(stmt));
  }
  return {};
}

std::vector<zigzag::CellRef>
VQLEngine::executePerspectiveBlock(const PerspectiveBlock &block) {
  std::string norm = block.perspectiveName;
  if (norm.starts_with('@')) {
    norm = norm.substr(1);
  }
  if (norm.ends_with(':')) {
    norm = norm.substr(0, norm.size() - 1);
  }

  PerspectiveScope scope(activePerspective_, std::move(norm));

  std::vector<zigzag::CellRef> lastResult;
  if (block.body) {
    for (const auto &stmt : block.body->statements) {
      lastResult = executeStatement(stmt);
    }
  }

  return lastResult;
}

std::vector<zigzag::CellRef>
VQLEngine::executeWatch(const WatchStatement &watchStmt) {
  static_cast<void>(watchStmt);
  throw std::runtime_error(
      "watch is unsupported: live subscriptions and historical catch-up are "
      "not implemented; use a normal query for a current snapshot");
}

void VQLEngine::executeMount(const MountDeclaration &mount) {
  sessionMounts_[mount.dimName] = mount;
  if (!mount.dimName.starts_with("d.")) {
    sessionMounts_["d." + mount.dimName] = mount;
  }
}

std::vector<zigzag::CellRef>
VQLEngine::resolveStoreAnchor(const StoreAnchor &sa) {
  std::vector<zigzag::CellRef> results;
  if (!sa.method.empty() && sa.method != "current") {
    throw std::runtime_error("unsupported store anchor: " + sa.method);
  }
  if (!sa.args.empty() || !sa.sliceName.empty()) {
    throw std::runtime_error(
        "unsupported store anchor arguments or slice selector");
  }
  if (const auto home = coordinator_.homeAnchor()) {
    results.push_back(*home);
  }
  if (sa.derefMaster) {
    for (auto &c : results) {
      this->arena().consumeWork();
      c = coordinator_.derefCloneMaster(c).value_or(c);
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

    PerspectiveScope scope(activePerspective_, normPersp);

    std::vector<zigzag::CellRef> res;
    if (cloneCell) {
      res = evaluateStep(targetStep, {*cloneCell});
    }
    if (res.empty()) {
      res = evaluateStep(targetStep, {c});
    }

    results.insert(results.end(), res.begin(), res.end());
  }
  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::resolveAnchor(const AnchorNode &anchor,
                         const std::vector<zigzag::CellRef> &contextCells) {
  if (anchor.versionPin) {
    throw std::runtime_error("version-pinned anchors are unsupported");
  }
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
    throw std::runtime_error("cursor anchor is unsupported");

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
    if (anchor.cellId == 0 ||
        anchor.cellId > std::numeric_limits<zigzag::CellRef>::max()) {
      throw std::runtime_error(
          "cell identity is zero or outside the CellRef range");
    }
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
    if (!anchor.generator || anchor.generator->opName != "current" ||
        !anchor.generator->args.empty()) {
      throw std::runtime_error("unsupported store generator: " +
                               (anchor.generator ? anchor.generator->opName
                                                 : std::string("missing")));
    }
    if (const auto home = coordinator_.homeAnchor()) {
      results.push_back(*home);
    }
    break;
  }
  case AnchorKind::StoreAnchor: {
    if (anchor.storeAnchor) {
      results = resolveStoreAnchor(*anchor.storeAnchor);
    }
    break;
  }
  case AnchorKind::Microversion:
    throw std::runtime_error("microversion anchors are unsupported");
  }

  if (anchor.derefMaster) {
    for (auto &c : results) {
      this->arena().consumeWork();
      c = coordinator_.derefCloneMaster(c).value_or(c);
    }
  }

  return results;
}

std::vector<zigzag::CellRef>
VQLEngine::evaluatePath(const PathExpression &path,
                        const std::vector<zigzag::CellRef> &contextCells) {
  if (path.shapeProjection) {
    throw std::runtime_error(
        "bare shape application is unsupported; use project(subject, shape) "
        "for a snapshot or apply_shape(subject, shape) for an explicit edit");
  }
  for (const auto &step : path.steps) {
    if (step.versionPin ||
        std::holds_alternative<DimensionPivotStep>(step.selector) ||
        std::holds_alternative<VirtualDimensionStep>(step.selector)) {
      throw std::runtime_error(
          "version pins, pivots and virtual dimensions are unsupported");
    }
  }
  std::vector<zigzag::CellRef> current =
      resolveAnchor(path.anchor, contextCells);
  std::erase_if(current, [&](const zigzag::CellRef cell) {
    return !std::ranges::all_of(
        path.anchorPredicates,
        [&](const BooleanExpr &pred) { return evaluatePredicate(pred, cell); });
  });

  for (std::size_t index = 0; index < path.steps.size(); ++index) {
    this->arena().consumeWork();
    const auto &step = path.steps[index];
    // Bare calls are represented as a contextual anchor and an initial step.
    // Only that initial invocation or a pure zero-argument count can start
    // without input. Later calls must not evaluate arguments on an empty walk.
    const auto *const emptyInputFunction =
        std::get_if<FunctionInvocation>(&step.selector);
    const bool emptyCount =
        emptyInputFunction && emptyInputFunction->name == "count" &&
        emptyInputFunction->args.empty() && !userDefs_.contains("count") &&
        !functorNamed("count");
    if (current.empty() && !emptyCount &&
        !(index == 0 && path.anchor.kind == AnchorKind::Context &&
          path.anchorPredicates.empty())) {
      break;
    }
    current = evaluateStep(step, current);
  }

  // Joining occupied ranks would silently transfer another content authority.
  if (path.cloneTail) {
    if (current.size() != 1) {
      throw std::runtime_error(
          "clone join requires exactly one leftmost authority");
    }
    const auto master = current.front();
    const auto dim    = core_->dims().clone;
    if (core_->arena().linkedMinting(master, dim, DimVector::NEG)) {
      throw std::runtime_error(
          "clone join leftmost cell is not a rank authority");
    }
    std::vector<zigzag::CellRef> additions;
    std::unordered_set<zigzag::CellRef> seen{master};
    for (const auto &operand : path.cloneTail->operands) {
      this->arena().consumeWork();
      std::vector<zigzag::CellRef> cells;
      if (operand.bareCreate) {
        const auto &create = *operand.bareCreate;
        if (create.expr) {
          if (const auto value = evaluateValueExpr(*create.expr, master)) {
            cells.push_back(cellFromValue(*value));
          }
        } else {
          cells.push_back(cellFromValue(create.literal));
        }
      } else if (operand.path) {
        cells = evaluatePath(*operand.path, contextCells);
      }
      for (const auto cell : cells) {
        if (!seen.insert(cell).second ||
            core_->arena().linkedMinting(cell, dim, DimVector::NEG) ||
            core_->arena().linkedMinting(cell, dim, DimVector::POS)) {
          throw std::runtime_error(
              "clone join collision: operand already belongs to a rank");
        }
        additions.push_back(cell);
      }
    }
    auto tail = zigzag::rankTail(core_->arena(), master, dim);
    for (const auto cell : additions) {
      this->arena().consumeWork();
      zigzag::expectWritten(
          core_->arena().link(tail, dim, DimVector::POS, cell));
      tail = cell;
    }
  }

  return current;
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
        auto mRoots = resolveStoreAnchor(mountIt->second.storeAnchor);
        results.insert(results.end(), mRoots.begin(), mRoots.end());
      }
    }
  }

  return results;
}

void VQLEngine::findInDocuments(const StoreInfo &info,
                                const std::string_view needle,
                                std::vector<zigzag::CellRef> &out) {
  // A document's prose is no cell, so a hit is answered with one: a quotation
  // of the line it is on -- the document's own spans, so the bytes stay in
  // the scroll they were typed into -- linked along d.source to where that
  // line was found.
  const auto identity =
      info.path.empty() ? "store:" + info.store->documentId().str() : info.path;
  const auto sourceDim = coordinator_.resolveDimension("d.source");
  auto &arena          = core_->arena();
  for (const auto &version : info.store->currentVersions()) {
    this->arena().consumeWork();
    const auto document = info.store->rebuild(version);
    const auto text     = info.store->textOf(version);
    for (auto at = text.find(needle); std::string::npos != at;) {
      this->arena().consumeWork();
      const auto newline =
          at == 0 ? std::string::npos : text.rfind('\n', at - 1);
      const auto begin = std::string::npos == newline ? 0 : newline + 1;
      const auto end   = std::min(text.find('\n', at), text.size());
      const auto spans =
          document.spansFor(static_cast<std::uint32_t>(begin),
                            static_cast<std::uint32_t>(end - begin));
      const auto hit    = arena.makeQuote(info.spaceId, spans);
      const auto source = arena.makeCell(
          identity + "#version=" + version.str() + "&at=" + std::to_string(at));
      zigzag::expectWritten(
          arena.link(hit, sourceDim, zigzag::DimVector::POS, source));
      out.push_back(hit);
      at = end < text.size() ? text.find(needle, end + 1) : std::string::npos;
    }
  }
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

std::vector<zigzag::CellRef>
VQLEngine::evaluateStep(const PathStep &step,
                        const std::vector<zigzag::CellRef> &currentCells) {
  if (step.versionPin ||
      std::holds_alternative<DimensionPivotStep>(step.selector) ||
      std::holds_alternative<VirtualDimensionStep>(step.selector)) {
    throw std::runtime_error(
        "version pins, pivots and virtual dimensions are unsupported");
  }
  // A function has an answer for nothing flowing in -- zero, what a search
  // finds anywhere, whether one string holds another -- except the two that
  // act on their inputs. A name nobody knows is still reported below.
  const auto *const function = std::get_if<FunctionInvocation>(&step.selector);
  const bool canStartFromEmpty =
      std::holds_alternative<GenesisSelector>(step.selector) ||
      std::holds_alternative<RootSelectorStep>(step.selector) ||
      std::holds_alternative<DocSelectorStep>(step.selector) ||
      std::holds_alternative<std::shared_ptr<PathLambda>>(step.selector) ||
      (std::holds_alternative<SignedDimensionStep>(step.selector) &&
       std::get<SignedDimensionStep>(step.selector).dimName.starts_with('$'));
  if (currentCells.empty() && !canStartFromEmpty &&
      (nullptr == function || "value" == function->name ||
       "link" == function->name)) {
    return {};
  }

  std::vector<zigzag::CellRef> stepOutput;

  if (std::holds_alternative<SignedDimensionStep>(step.selector)) {
    const auto &s = std::get<SignedDimensionStep>(step.selector);
    const auto functor =
        s.dimName.starts_with('$')
            ? functorNamed(std::string_view(s.dimName).substr(1))
            : std::nullopt;
    if (s.dimName.starts_with('$') && !functor) {
      std::string varName = s.dimName.substr(1);
      auto it             = env_.find(varName);
      if (it != env_.end()) {
        stepOutput = it->second;
        return stepOutput;
      }
    }
    if (functor) {
      // `$c / $up`: the functor's steps run over this step's input, and the
      // step's own predicates and clamp then apply to what they reach.
      stepOutput = applyFunctor(**functor, currentCells);
    } else if (!activePerspective_.empty() && !currentCells.empty() &&
               s.creates.empty()) {
      for (zigzag::CellRef c : currentCells) {
        this->arena().consumeWork();
        std::optional<zigzag::CellRef> cloneCell;
        auto itP = perspectiveClones_.find(activePerspective_);
        if (itP != perspectiveClones_.end()) {
          auto itC = itP->second.find(c);
          if (itC != itP->second.end()) {
            cloneCell = itC->second;
          }
        }
        std::vector<zigzag::CellRef> singleRes;
        if (cloneCell) {
          singleRes = traverseDimension(s, {*cloneCell});
        }
        if (singleRes.empty()) {
          singleRes = traverseDimension(s, {c});
        }
        stepOutput.insert(stepOutput.end(), singleRes.begin(), singleRes.end());
      }
    } else if (s.creates.empty()) {
      stepOutput = traverseDimension(s, currentCells);
    } else {
      stepOutput = performCreates(s, currentCells);
      // Yield mode mapping on creates:
      if (step.yieldMode == YieldMode::Last) {
        if (!stepOutput.empty()) {
          stepOutput = {stepOutput.back()};
        }
      } else if (step.yieldMode == YieldMode::Both) {
        stepOutput = concatCellStreams(currentCells, stepOutput);
      } else if (step.yieldMode == YieldMode::Keep) {
        stepOutput = currentCells;
      }
    }
  } else if (std::holds_alternative<std::shared_ptr<MacroDimensionGroup>>(
                 step.selector)) {
    const auto &mg =
        *std::get<std::shared_ptr<MacroDimensionGroup>>(step.selector);
    auto runOnce = [&](std::vector<zigzag::CellRef> in) {
      for (const auto &innerStep : mg.steps) {
        this->arena().consumeWork();
        if (in.empty()) {
          break;
        }
        in = evaluateStep(innerStep, in);
      }
      return in;
    };

    if (mg.repetition == Repetition::Once) {
      stepOutput = runOnce(currentCells);
    } else if (mg.repetition == Repetition::ZeroOrOne) {
      auto res   = runOnce(currentCells);
      stepOutput = res.empty() ? currentCells : res;
    } else if (mg.repetition == Repetition::ZeroOrMore ||
               mg.repetition == Repetition::OneOrMore) {
      std::vector<zigzag::CellRef> cur = currentCells;
      if (mg.repetition == Repetition::OneOrMore) {
        cur = runOnce(cur);
      }
      int iters = 0;
      while (iters++ < 1000) {
        this->arena().consumeWork();
        auto next = runOnce(cur);
        if (next == cur || next.empty()) {
          break;
        }
        cur = std::move(next);
      }
      stepOutput = cur;
    }
  } else if (std::holds_alternative<FunctionInvocation>(step.selector)) {
    const auto &fn = std::get<FunctionInvocation>(step.selector);
    if (fn.name == "value") {
      if (fn.args.size() > 3) {
        throw std::runtime_error("value accepts at most three arguments; got " +
                                 std::to_string(fn.args.size()));
      }
      if (currentCells.empty()) {
        return {};
      }

      std::int64_t offset = 0;
      std::optional<std::int64_t> length;
      if (fn.args.size() >= 2) {
        const auto offsetValue = evaluateValueExpr(fn.args[1], currentCells[0]);
        if (!offsetValue) return stepOutput;
        if (std::holds_alternative<std::int64_t>(*offsetValue)) {
          offset = std::get<std::int64_t>(*offsetValue);
        }
      }
      if (fn.args.size() >= 3) {
        const auto lengthValue = evaluateValueExpr(fn.args[2], currentCells[0]);
        if (!lengthValue) return stepOutput;
        if (std::holds_alternative<std::int64_t>(*lengthValue)) {
          length = std::get<std::int64_t>(*lengthValue);
        }
      }
      for (zigzag::CellRef c : currentCells) {
        this->arena().consumeWork();
        if (const auto res = core_->readValue(c, offset, length)) {
          stepOutput.push_back(*res);
        }
      }
    } else if (fn.name == "find") {
      if (fn.args.size() != 1) {
        throw std::runtime_error("find requires one argument; got " +
                                 std::to_string(fn.args.size()));
      }
      // Every cell of every loaded store whose text holds the needle: how a
      // reader finds something without knowing where it is. The audit found
      // no way to search text at all.
      const auto needleValue = evaluateValueExpr(fn.args[0], std::nullopt);
      if (!needleValue) return stepOutput;
      const auto needle = textOfValue(*needleValue);
      for (const auto &info : coordinator_.stores()) {
        this->arena().consumeWork();
        if (!info.manifold || !info.store) {
          continue;
        }
        for (const auto &slot : info.manifold->cells()) {
          this->arena().consumeWork();
          if (info.manifold->textOf(slot.birthOp, *info.store).find(needle) !=
              std::string::npos) {
            stepOutput.push_back(
                core_->arena().proxyFor(info.spaceId, slot.birthOp));
          }
        }
        findInDocuments(info, needle, stepOutput);
      }
    } else if (fn.name == "contains") {
      if (fn.args.size() != 2) {
        throw std::runtime_error("contains requires two arguments; got " +
                                 std::to_string(fn.args.size()));
      }
      // The predicate's answer, as a query of its own: true or false.
      const std::optional<zigzag::CellRef> context =
          currentCells.empty()
              ? std::nullopt
              : std::optional<zigzag::CellRef>{currentCells.front()};
      const auto haystackValue = evaluateValueExpr(fn.args[0], context);
      const auto needleValue   = evaluateValueExpr(fn.args[1], context);
      const bool contains =
          haystackValue && needleValue &&
          std::string::npos !=
              textOfValue(*haystackValue).find(textOfValue(*needleValue));
      stepOutput.push_back(core_->arena().makeScalarCell(contains));
    } else if (fn.name == "count") {
      std::size_t counted = currentCells.size();
      if (fn.args.size() > 1) {
        throw std::runtime_error("count accepts zero or one argument");
      }
      if (!fn.args.empty()) {
        const std::optional<zigzag::CellRef> context =
            currentCells.empty()
                ? std::nullopt
                : std::optional<zigzag::CellRef>{currentCells.front()};
        counted = cellsOfValueExpr(fn.args.front(), context).size();
      }
      stepOutput.push_back(
          core_->arena().makeScalarCell(static_cast<std::int64_t>(counted)));
    } else if (fn.name == "link") {
      if (fn.args.size() != 2) {
        throw std::runtime_error(
            "link requires a signed dimension and an existing cell target");
      }
      if (std::holds_alternative<ScalarLiteral>(fn.args[1].kind)) {
        throw std::runtime_error(
            "link requires cell identity, not a scalar or allocation/isolation "
            "sentinel; use % to create a cell or apply_shape to remove links");
      }
      // Existing-Target Fan-Out (§4.7)
      // Resolve dimension and direction from arg 0
      std::string dimName;
      DimVector direction = DimVector::POS;
      if (std::holds_alternative<ScalarLiteral>(fn.args[0].kind)) {
        const auto &lit = std::get<ScalarLiteral>(fn.args[0].kind);
        if (std::holds_alternative<std::string>(lit.value)) {
          std::string str = std::get<std::string>(lit.value);
          if (!str.empty() && str[0] == '-') {
            direction = DimVector::NEG;
            dimName   = str.substr(1);
          } else if (!str.empty() && str[0] == '+') {
            direction = DimVector::POS;
            dimName   = str.substr(1);
          } else {
            dimName = str;
          }
        }
      } else if (std::holds_alternative<std::shared_ptr<PathExpression>>(
                     fn.args[0].kind)) {
        const auto &p =
            *std::get<std::shared_ptr<PathExpression>>(fn.args[0].kind);
        if (!p.steps.empty() &&
            std::holds_alternative<SignedDimensionStep>(p.steps[0].selector)) {
          const auto &sd = std::get<SignedDimensionStep>(p.steps[0].selector);
          dimName        = sd.dimName;
          direction      = sd.direction;
        }
      }

      if (dimName.empty()) {
        throw std::runtime_error("link requires a signed dimension");
      }
      const auto targets = cellsOfValueExpr(
          fn.args[1], currentCells.empty() ? std::nullopt
                                           : std::optional<zigzag::CellRef>{
                                                 currentCells.front()});
      if (targets.size() != 1) {
        throw std::runtime_error(
            "link requires exactly one existing target cell");
      }
      const auto targetCell = targets.front();
      const auto dim        = coordinator_.resolveDimension(dimName);

      {
        auto gen = core_->cloneGenerator(targetCell);
        std::vector<zigzag::CellRef> attachedClones;
        for (zigzag::CellRef c : currentCells) {
          this->arena().consumeWork();
          zigzag::CellRef cloneCell = gen();
          core_->setLink(c, dim, direction, cloneCell);
          attachedClones.push_back(cloneCell);
        }

        if (step.yieldMode == YieldMode::Both) {
          stepOutput = concatCellStreams(currentCells, attachedClones);
        } else if (step.yieldMode == YieldMode::Keep) {
          stepOutput = currentCells;
        } else if (step.yieldMode == YieldMode::Last) {
          if (!attachedClones.empty()) {
            stepOutput = {attachedClones.back()};
          }
        } else {
          stepOutput = attachedClones;
        }
      }
    } else if (fn.name == "delta") {
      throw std::runtime_error("delta is unsupported without live observation");
    } else if (isSemanticBuiltin(fn.name) && !userDefs_.contains(fn.name)) {
      stepOutput = evaluateSemanticBuiltin(fn, currentCells);

    } else if (auto itDef = userDefs_.find(fn.name); itDef != userDefs_.end()) {
      if (currentCells.empty()) {
        auto res = callUserDef(itDef->second, fn.args, std::nullopt);
        stepOutput.insert(stepOutput.end(), res.begin(), res.end());
      } else {
        for (zigzag::CellRef c : currentCells) {
          std::vector<ValueExpr> callArgs;
          if (itDef->second.params.size() > fn.args.size()) {
            callArgs.push_back(ValueExpr{
                .kind = std::make_shared<PathExpression>(
                    PathExpression{.anchor = AnchorNode{
                                       .kind   = AnchorKind::LiteralCellId,
                                       .cellId = c,
                                   }})});
            callArgs.insert(callArgs.end(), fn.args.begin(), fn.args.end());
          } else {
            callArgs = fn.args;
          }
          auto res = callUserDef(itDef->second, callArgs, c);
          stepOutput.insert(stepOutput.end(), res.begin(), res.end());
        }
      }
    } else {
      // Answering nothing for a name it does not know reads as "no match".
      throw std::runtime_error("no function " + fn.name + " taking " +
                               std::to_string(fn.args.size()) + " argument(s)");
    }
  } else if (std::holds_alternative<GenesisSelector>(step.selector)) {
    if (currentCells.empty()) {
      if (const auto h = coordinator_.homeAnchor()) {
        stepOutput.push_back(*h);
      }
    } else {
      for (zigzag::CellRef c : currentCells) {
        if (const auto home = sliceHomeOf(c)) stepOutput.push_back(*home);
      }
    }
  } else if (std::holds_alternative<RootSelectorStep>(step.selector)) {
    const auto &rs = std::get<RootSelectorStep>(step.selector);
    switch (rs.kind) {
    case RootSelectorStep::Kind::Genesis:
      if (currentCells.empty()) {
        if (const auto h = coordinator_.homeAnchor()) {
          stepOutput.push_back(*h);
        }
      } else {
        for (zigzag::CellRef c : currentCells) {
          if (const auto home = sliceHomeOf(c)) stepOutput.push_back(*home);
        }
      }
      break;

    case RootSelectorStep::Kind::RootMetacells:
      if (currentCells.empty()) {
        if (const auto h = coordinator_.homeAnchor()) {
          stepOutput.push_back(*h);
        }
      } else {
        zigzag::DimRef dMeta = coordinator_.resolveDimension("d.meta");
        for (zigzag::CellRef c : currentCells) {
          if (const auto sh = sliceHomeOf(c)) {
            const auto meta = core_->arena().linkedMinting(
                *sh, dMeta, zigzag::DimVector::POS);
            stepOutput.push_back(meta ? *meta : *sh);
          }
        }
      }
      break;

    case RootSelectorStep::Kind::ActiveCursor: {
      const auto dCursors = core_->dims().cursors;
      if (currentCells.empty()) {
        stepOutput.push_back(
            core_->arena()
                .linkedMinting(core_->home(), dCursors, zigzag::DimVector::POS)
                .value_or(dCursors));
      } else {
        for (const auto c : currentCells) {
          if (const auto home = sliceHomeOf(c)) {
            stepOutput.push_back(
                core_->arena()
                    .linkedMinting(*home, dCursors, zigzag::DimVector::POS)
                    .value_or(dCursors));
          }
        }
      }
      break;
    }

    case RootSelectorStep::Kind::AllCursors: {
      zigzag::DimRef dCursors = core_->dims().cursors;
      std::vector<zigzag::CellRef> bases;
      if (currentCells.empty()) {
        bases.push_back(core_->home());
      } else {
        for (zigzag::CellRef c : currentCells) {
          if (const auto base = sliceHomeOf(c)) bases.push_back(*base);
        }
      }
      for (zigzag::CellRef base : bases) {

        auto cur = core_->arena().linkedMinting(base, dCursors,
                                                zigzag::DimVector::POS);
        std::unordered_set<zigzag::CellRef> seen;
        while (cur && seen.insert(*cur).second) {
          stepOutput.push_back(*cur);
          cur = core_->arena().linkedMinting(*cur, dCursors,
                                             zigzag::DimVector::POS);
        }
      }
      break;
    }

    case RootSelectorStep::Kind::NamedCursor: {
      zigzag::DimRef dCursors = core_->dims().cursors;
      zigzag::DimRef dName    = coordinator_.dimName();
      std::vector<zigzag::CellRef> bases;
      if (currentCells.empty()) {
        bases.push_back(core_->home());
      } else {
        for (zigzag::CellRef c : currentCells) {
          if (const auto base = sliceHomeOf(c)) bases.push_back(*base);
        }
      }
      for (zigzag::CellRef base : bases) {

        auto cur = core_->arena().linkedMinting(base, dCursors,
                                                zigzag::DimVector::POS);
        std::unordered_set<zigzag::CellRef> seen;
        while (cur && seen.insert(*cur).second) {
          std::string name = core_->arena().textOf(*cur);
          if (name != rs.name) {
            const auto nameCell = core_->arena().linkedMinting(
                *cur, dName, zigzag::DimVector::POS);
            if (nameCell) {
              name = core_->arena().textOf(*nameCell);
            }
          }
          if (name == rs.name) {
            stepOutput.push_back(*cur);
            break;
          }
          cur = core_->arena().linkedMinting(*cur, dCursors,
                                             zigzag::DimVector::POS);
        }
      }
      break;
    }

    case RootSelectorStep::Kind::AllNamedCursors: {
      zigzag::DimRef dCursors = core_->dims().cursors;
      zigzag::DimRef dName    = coordinator_.dimName();
      std::vector<zigzag::CellRef> bases;
      if (currentCells.empty()) {
        bases.push_back(core_->home());
      } else {
        for (zigzag::CellRef c : currentCells) {
          if (const auto base = sliceHomeOf(c)) bases.push_back(*base);
        }
      }
      for (zigzag::CellRef base : bases) {

        auto cur = core_->arena().linkedMinting(base, dCursors,
                                                zigzag::DimVector::POS);
        std::unordered_set<zigzag::CellRef> seen;
        while (cur && seen.insert(*cur).second) {
          std::string name = core_->arena().textOf(*cur);
          if (name != rs.name) {
            const auto nameCell = core_->arena().linkedMinting(
                *cur, dName, zigzag::DimVector::POS);
            if (nameCell) {
              name = core_->arena().textOf(*nameCell);
            }
          }
          if (name == rs.name) {
            stepOutput.push_back(*cur);
          }
          cur = core_->arena().linkedMinting(*cur, dCursors,
                                             zigzag::DimVector::POS);
        }
      }
      break;
    }
    }
  } else if (std::holds_alternative<PerspectiveStep>(step.selector)) {
    const auto &pStep = std::get<PerspectiveStep>(step.selector);
    if (pStep.targetStep) {
      stepOutput = evaluatePerspectiveStep(pStep.perspective, *pStep.targetStep,
                                           currentCells);
    }
  } else if (std::holds_alternative<SliceSelectorStep>(step.selector)) {
    const auto &sl = std::get<SliceSelectorStep>(step.selector);
    for (const auto &info : coordinator_.stores()) {
      if (info.manifold && info.homeCell) {
        const zigzag::CellRef h = *info.homeCell;
        if (sl.nameFilter) {
          if (info.label == *sl.nameFilter || *sl.nameFilter == "all") {
            stepOutput.push_back(h);
          }
        } else if (sl.predicateFilter) {
          if (evaluatePredicate(*sl.predicateFilter, h)) {
            stepOutput.push_back(h);
          }
        } else {
          stepOutput.push_back(h);
        }
      }
    }
  } else if (std::holds_alternative<DocSelectorStep>(step.selector)) {
    const auto &ds = std::get<DocSelectorStep>(step.selector);
    for (const auto &info : coordinator_.stores()) {
      if (info.manifold && info.homeCell) {
        const zigzag::CellRef h = *info.homeCell;
        if (ds.nameFilter) {
          if (info.label == *ds.nameFilter || *ds.nameFilter == "all") {
            stepOutput.push_back(h);
          }
        } else if (ds.predicateFilter) {
          if (evaluatePredicate(*ds.predicateFilter, h)) {
            stepOutput.push_back(h);
          }
        } else {
          stepOutput.push_back(h);
        }
      }
    }
  } else if (std::holds_alternative<PathMorphism>(step.selector)) {
    const auto &m         = std::get<PathMorphism>(step.selector);
    std::string perspName = m.perspective;
    if (perspName.empty()) {
      perspName = activePerspective_;
    }
    if (perspName.starts_with('@')) {
      perspName = perspName.substr(1);
    }
    if (perspName.ends_with(':')) {
      perspName = perspName.substr(0, perspName.size() - 1);
    }

    std::string dimName         = "d.1";
    zigzag::DimVector direction = zigzag::DimVector::POS;

    auto extractDim = [&](const std::string &str) {
      if (str.starts_with('+')) {
        direction = zigzag::DimVector::POS;
        dimName   = str.substr(1);
      } else if (str.starts_with('-')) {
        direction = zigzag::DimVector::NEG;
        dimName   = str.substr(1);
      } else if (!str.empty()) {
        direction = zigzag::DimVector::POS;
        dimName   = str;
      }
    };

    if (std::holds_alternative<ScalarLiteral>(m.subject.kind)) {
      const auto &lit = std::get<ScalarLiteral>(m.subject.kind);
      if (std::holds_alternative<std::string>(lit.value)) {
        extractDim(std::get<std::string>(lit.value));
      }
    } else if (std::holds_alternative<std::string>(m.subject.kind)) {
      extractDim(std::get<std::string>(m.subject.kind));
    } else if (std::holds_alternative<std::shared_ptr<PathExpression>>(
                   m.subject.kind)) {
      const auto &p = std::get<std::shared_ptr<PathExpression>>(m.subject.kind);
      if (p != nullptr && !p->steps.empty() &&
          std::holds_alternative<SignedDimensionStep>(p->steps[0].selector)) {
        const auto &sd = std::get<SignedDimensionStep>(p->steps[0].selector);
        dimName        = sd.dimName;
        direction      = sd.direction;
      }
    }

    zigzag::DimRef dim = resolveDimension(dimName);

    for (zigzag::CellRef c : currentCells) {
      this->arena().consumeWork();
      for (const auto &tgt : m.targets) {
        this->arena().consumeWork();
        const auto value = evaluateValueExpr(tgt, c);
        if (!value) continue;
        const zigzag::CellRef sprout = cellFromValue(*value);

        if (!perspName.empty()) {
          auto &clones = perspectiveClones_[perspName];
          zigzag::CellRef cloneCell;
          auto it = clones.find(c);
          if (it != clones.end()) {
            cloneCell = it->second;
          } else {
            cloneCell             = core_->arena().makeCell();
            zigzag::DimRef dClone = core_->dims().clone;
            zigzag::CellRef tail  = zigzag::rankTail(core_->arena(), c, dClone);
            zigzag::expectWritten(core_->arena().link(
                tail, dClone, zigzag::DimVector::POS, cloneCell));
            zigzag::expectWritten(core_->arena().link(
                cloneCell, dClone, zigzag::DimVector::NEG, tail));
            clones[c] = cloneCell;
          }

          zigzag::DimVector oppDir = (direction == zigzag::DimVector::POS)
                                         ? zigzag::DimVector::NEG
                                         : zigzag::DimVector::POS;
          zigzag::expectWritten(
              core_->arena().link(cloneCell, dim, direction, sprout));
          zigzag::expectWritten(
              core_->arena().link(sprout, dim, oppDir, cloneCell));
          stepOutput.push_back(sprout);
        } else {
          zigzag::DimVector oppDir = (direction == zigzag::DimVector::POS)
                                         ? zigzag::DimVector::NEG
                                         : zigzag::DimVector::POS;
          zigzag::expectWritten(core_->arena().link(c, dim, direction, sprout));
          zigzag::expectWritten(core_->arena().link(sprout, dim, oppDir, c));
          stepOutput.push_back(sprout);
        }
      }
    }
  } else if (std::holds_alternative<DimensionPivotStep>(step.selector)) {
    stepOutput = currentCells;
  } else if (std::holds_alternative<VirtualDimensionStep>(step.selector)) {
    const auto &v = std::get<VirtualDimensionStep>(step.selector);
    for (zigzag::CellRef c : currentCells) {
      if (const auto val = evaluateValueExpr(v.expr, c)) {
        stepOutput.push_back(cellFromValue(*val));
      }
    }

  } else if (std::holds_alternative<std::shared_ptr<PathLambda>>(
                 step.selector)) {
    const auto &lambda = *std::get<std::shared_ptr<PathLambda>>(step.selector);
    if (currentCells.empty()) {
      auto res = executeLambda(lambda, std::nullopt);
      stepOutput.insert(stepOutput.end(), res.begin(), res.end());
    } else {
      for (zigzag::CellRef c : currentCells) {
        auto res = executeLambda(lambda, c);
        stepOutput.insert(stepOutput.end(), res.begin(), res.end());
      }
    }
  }

  // Master Dereference >
  if (step.derefMaster) {
    for (auto &c : stepOutput) {
      this->arena().consumeWork();
      c = coordinator_.derefCloneMaster(c).value_or(c);
    }
  }

  // Predicates Filtering
  if (!step.predicates.empty()) {
    std::vector<zigzag::CellRef> filtered;
    for (zigzag::CellRef c : stepOutput) {
      this->arena().consumeWork();
      bool allPassed = true;
      for (const auto &pred : step.predicates) {
        this->arena().consumeWork();
        if (!evaluatePredicate(pred, c)) {
          allPassed = false;
          break;
        }
      }
      if (allPassed) {
        filtered.push_back(c);
      }
    }
    stepOutput = std::move(filtered);
  }

  // Shape Predicates Filtering [{ ... }]
  if (!step.shapePredicates.empty()) {
    std::vector<zigzag::CellRef> shapeFiltered;
    for (zigzag::CellRef c : stepOutput) {
      this->arena().consumeWork();
      bool allPassed = true;
      for (const auto &shape : step.shapePredicates) {
        this->arena().consumeWork();
        if (!evaluateShapePredicate(shape, c)) {
          allPassed = false;
          break;
        }
      }
      if (allPassed) {
        shapeFiltered.push_back(c);
      }
    }
    stepOutput = std::move(shapeFiltered);
  }

  // Range Clamp [start, end]
  if (step.rangeClamp.has_value() && !stepOutput.empty()) {
    const auto &clamp = *step.rangeClamp;
    auto total        = static_cast<std::int64_t>(stepOutput.size());
    std::int64_t s    = clamp.start;
    std::int64_t e    = clamp.end.value_or(s);

    std::int64_t from = (s < 0) ? (total + s) : (s - 1);
    std::int64_t to   = (e < 0) ? (total + e) : (e - 1);

    from = std::clamp<std::int64_t>(from, 0, total - 1);
    to   = std::clamp<std::int64_t>(to, 0, total - 1);

    if (from <= to) {
      stepOutput = std::vector<zigzag::CellRef>(stepOutput.begin() + from,
                                                stepOutput.begin() + to + 1);
    } else {
      stepOutput.clear();
    }
  }

  if (step.yieldMode == YieldMode::Keep) {
    return currentCells;
  }

  return stepOutput;
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
    if (fn.name == "contains") {
      if (fn.args.size() != 2) {
        throw std::runtime_error("contains requires two arguments; got " +
                                 std::to_string(fn.args.size()));
      }
      const auto haystack = evaluateValueExpr(fn.args[0], context);
      const auto needle   = evaluateValueExpr(fn.args[1], context);
      if (!haystack || !needle) return zigzag::vortex::CellValue{false};
      return zigzag::vortex::CellValue{
          std::string::npos !=
          textOfValue(*haystack).find(textOfValue(*needle))};
    }
    if (fn.name == "count") {
      if (fn.args.size() > 1) {
        throw std::runtime_error("count accepts zero or one argument");
      }
      return static_cast<std::int64_t>(
          fn.args.empty() ? (context ? 1 : 0)
                          : cellsOfValueExpr(fn.args.front(), context).size());
    }
    if (fn.name == "delta") {
      throw std::runtime_error("delta is unsupported without live observation");
    }
    if (isSemanticBuiltin(fn.name) && !userDefs_.contains(fn.name)) {
      auto cells = evaluateSemanticBuiltin(
          fn, context ? std::vector<zigzag::CellRef>{*context}
                      : std::vector<zigzag::CellRef>{});
      if (cells.empty()) return std::nullopt;
      return core_->render(cells.front());
    }

    if (const auto itDef = userDefs_.find(fn.name); itDef != userDefs_.end()) {
      const auto results = callUserDef(itDef->second, fn.args, context);
      if (results.empty()) return std::nullopt;
      return core_->render(results.front());
    }
    throw std::runtime_error("no scalar function " + fn.name + " taking " +
                             std::to_string(fn.args.size()) + " argument(s)");
  }

  if (std::holds_alternative<std::shared_ptr<PathLambda>>(expr.kind)) {
    const auto &lambda = *std::get<std::shared_ptr<PathLambda>>(expr.kind);
    const auto results = executeLambda(lambda, context);
    if (results.empty()) return std::nullopt;
    return core_->render(results.front());
  }

  if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(expr.kind)) {
    throw std::runtime_error(
        "shape values are unsupported outside shape predicates and explicit "
        "project/apply_shape masks");
  }

  if (std::holds_alternative<std::shared_ptr<VersionExpr>>(expr.kind)) {
    const auto &ve = *std::get<std::shared_ptr<VersionExpr>>(expr.kind);
    return zigzag::vortex::CellValue{ve.versionStr};
  }

  if (std::holds_alternative<std::shared_ptr<BooleanExpr>>(expr.kind)) {
    const auto &b = *std::get<std::shared_ptr<BooleanExpr>>(expr.kind);
    return zigzag::vortex::CellValue{evaluatePredicate(b, context)};
  }

  return std::nullopt;
}

bool VQLEngine::isSemanticBuiltin(const std::string_view name) {
  return name == "project" || name == "apply_shape" || name == "any" ||
         name == "all" || name == "none" || name == "one" ||
         name == "optional_one" || name == "first" || name == "same_cell";
}

std::vector<zigzag::CellRef> VQLEngine::evaluateSemanticBuiltin(
    const FunctionInvocation &fn,
    const std::vector<zigzag::CellRef> &contextCells) {
  const std::optional<zigzag::CellRef> context =
      contextCells.empty()
          ? std::nullopt
          : std::optional<zigzag::CellRef>{contextCells.front()};
  const auto requireArity = [&](const std::size_t count) {
    if (fn.args.size() != count) {
      throw std::runtime_error(fn.name + " requires " + std::to_string(count) +
                               " argument(s)");
    }
  };
  if (fn.name == "project" || fn.name == "apply_shape") {
    requireArity(2);
    const auto *shape =
        std::get_if<std::shared_ptr<ShapeLiteral>>(&fn.args[1].kind);
    if (!shape || !*shape) {
      throw std::runtime_error(fn.name + " requires a literal shape");
    }
    return applyShape(cellsOfValueExpr(fn.args[0], context), **shape,
                      fn.name == "apply_shape");
  }
  if (fn.name == "same_cell") {
    requireArity(2);
    const auto left  = cellsOfValueExpr(fn.args[0], context);
    const auto right = cellsOfValueExpr(fn.args[1], context);
    if (left.size() != 1 || right.size() != 1) {
      throw std::runtime_error("same_cell requires two singleton cell streams");
    }
    return {core_->arena().makeScalarCell(left.front() == right.front())};
  }
  if (fn.name == "one" || fn.name == "optional_one" || fn.name == "first") {
    requireArity(1);
    auto cells = cellsOfValueExpr(fn.args[0], context);
    if ((fn.name == "one" && cells.size() != 1) ||
        (fn.name == "optional_one" && cells.size() > 1)) {
      throw std::runtime_error(fn.name + " cardinality violation: got " +
                               std::to_string(cells.size()) + " cells");
    }
    if (fn.name == "first" && !cells.empty()) {
      cells.resize(1);
    }
    return cells;
  }
  requireArity(2);
  const auto domain = cellsOfValueExpr(fn.args[0], context);
  bool result       = fn.name != "any";
  const auto savedContext =
      env_.find(".") != env_.end() ? std::optional(env_.at(".")) : std::nullopt;
  const auto restore = [&] {
    if (savedContext) {
      env_["."] = *savedContext;
    } else {
      env_.erase(".");
    }
  };
  try {
    for (const auto cell : domain) {
      this->arena().consumeWork();
      env_["."]    = {cell};
      bool matched = false;
      if (const auto *path =
              std::get_if<std::shared_ptr<PathExpression>>(&fn.args[1].kind)) {
        matched = !evaluatePath(**path, {cell}).empty();
      } else if (const auto *shape = std::get_if<std::shared_ptr<ShapeLiteral>>(
                     &fn.args[1].kind)) {
        matched = evaluateShapePredicate(**shape, cell);
      } else if (const auto *lambda = std::get_if<std::shared_ptr<PathLambda>>(
                     &fn.args[1].kind)) {
        const auto answers = executeLambda(**lambda, cell);
        matched = std::ranges::any_of(answers, [&](const auto answer) {
          return core_->isTruthy(answer).value_or(false);
        });
      } else if (const auto *name = std::get_if<std::string>(&fn.args[1].kind);
                 name && functorNamed(*name)) {
        const auto functor = functorNamed(*name);
        matched            = !applyFunctor(**functor, {cell}).empty();
      } else {
        if (const auto value = evaluateValueExpr(fn.args[1], cell)) {
          matched = zigzag::vortex::VortexCore::evaluateTruthiness(*value);
        }
      }
      if (fn.name == "all" && !matched) {
        result = false;
        break;
      }
      if (matched && fn.name != "all") {
        result = fn.name == "any";
        break;
      }
    }
  } catch (...) {
    restore();
    throw;
  }
  restore();
  return {core_->arena().makeScalarCell(result)};
}

std::vector<zigzag::CellRef>
VQLEngine::applyShape(const std::vector<zigzag::CellRef> &subjects,
                      const ShapeLiteral &shape, const bool edit) {
  using Edge = std::pair<zigzag::DimRef, DimVector>;
  std::vector<Edge> selected;
  std::vector<std::pair<Edge, const ValueExpr *>> targetRules;
  auto &arena     = core_->arena();
  auto dimensions = arena.dimensions();
  // Core-minted dimensions can be incident edges without an arena name entry.
  for (const auto &cell : arena.cells()) {
    this->arena().consumeWork();
    for (const auto &edge : arena.dimensionsOf(cell.birthOp)) {
      if (std::ranges::find(dimensions, edge.dim) == dimensions.end()) {
        dimensions.push_back(edge.dim);
      }
    }
  }
  for (const auto &rule : shape.rules) {
    if (!std::holds_alternative<ShapeWildcard>(rule.rule) &&
        !std::holds_alternative<DimensionShapeRule>(rule.rule)) {
      throw std::runtime_error("unsupported shape rule: only wildcards and "
                               "dimensions are implemented");
    }
  }
  for (const auto &rule : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&rule.rule)) {
      selected.clear();
      targetRules.clear();
      if (*wildcard == ShapeWildcard::Everything) {
        for (const auto dim : dimensions) {
          selected.emplace_back(dim, DimVector::POS);
          selected.emplace_back(dim, DimVector::NEG);
        }
      }
    }
    if (const auto *dim = std::get_if<DimensionShapeRule>(&rule.rule)) {
      if (!edit && dim->targetValue) {
        throw std::runtime_error("project accepts dimension masks; use "
                                 "apply_shape for target assignments");
      }
      if (dim->targetValue && (dim->negated || dim->isBidir)) {
        throw std::runtime_error(
            "shape assignments require one signed dimension rule");
      }
      const auto add = [&](const DimVector direction) {
        const Edge edge{resolveDimension(dim->dimName), direction};
        std::erase(selected, edge);
        std::erase_if(targetRules,
                      [&](const auto &rule) { return rule.first == edge; });
        if (!dim->negated) {
          selected.push_back(edge);
          if (dim->targetValue) {
            targetRules.emplace_back(edge, dim->targetValue.get());
          }
        }
      };
      add(dim->direction);
      if (dim->isBidir) {
        add(dim->direction == DimVector::POS ? DimVector::NEG : DimVector::POS);
      }
    }
  }
  if (edit) {
    struct Assignment {
      zigzag::CellRef subject;
      Edge edge;
      zigzag::CellRef target;
    };
    std::vector<Assignment> assignments;
    const auto validateTarget = [&](const auto &validate,
                                    const ValueExpr &value) -> void {
      this->arena().consumeWork();
      if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(value.kind)) {
        throw std::runtime_error("nested shape assignment is unsupported");
      }
      if (const auto *function =
              std::get_if<std::shared_ptr<FunctionInvocation>>(&value.kind)) {
        for (std::size_t index = 0; index < (*function)->args.size(); ++index) {
          const auto &argument = (*function)->args[index];
          const bool shapeConsumer =
              index == 1 && !userDefs_.contains((*function)->name) &&
              ((*function)->name == "project" ||
               (*function)->name == "apply_shape" ||
               (*function)->name == "any" || (*function)->name == "all" ||
               (*function)->name == "none");
          if (shapeConsumer &&
              std::holds_alternative<std::shared_ptr<ShapeLiteral>>(
                  argument.kind)) {
            continue;
          }
          validate(validate, argument);
        }
      }
    };
    for (const auto &[edge, target] : targetRules) {
      static_cast<void>(edge);
      validateTarget(validateTarget, *target);
    }
    // Resolve and validate every explicit target before severing any edge.
    for (const auto subject : subjects) {
      for (const auto &[edge, target] : targetRules) {
        const auto targets = cellsOfValueExpr(*target, subject);
        if (targets.size() != 1) {
          throw std::runtime_error(
              "shape assignment requires exactly one target");
        }
        assignments.push_back({subject, edge, targets.front()});
      }
    }
    using Slot = std::tuple<zigzag::CellRef, zigzag::DimRef, DimVector>;
    std::map<Slot, zigzag::CellRef> requested;
    const auto claim = [&](const Slot &slot, const zigzag::CellRef partner) {
      const auto [found, inserted] = requested.emplace(slot, partner);
      if (!inserted && found->second != partner) {
        throw std::runtime_error(
            "shape assignment collision: incompatible reciprocal targets");
      }
    };
    for (const auto &assignment : assignments) {
      this->arena().consumeWork();
      const auto [dim, direction] = assignment.edge;
      claim({assignment.subject, dim, direction}, assignment.target);
      claim({assignment.target, dim,
             direction == DimVector::POS ? DimVector::NEG : DimVector::POS},
            assignment.subject);
    }
    for (const auto subject : subjects) {
      for (const auto dim : dimensions) {
        for (const auto direction : {DimVector::POS, DimVector::NEG}) {
          this->arena().consumeWork();
          if (std::ranges::find(selected, Edge{dim, direction}) ==
              selected.end()) {
            zigzag::expectWritten(arena.unlink(subject, dim, direction));
          }
        }
      }
    }
    for (const auto &assignment : assignments) {
      zigzag::expectWritten(
          arena.link(assignment.subject, assignment.edge.first,
                     assignment.edge.second, assignment.target));
    }
    return subjects;
  }

  // Snapshot copies keep all edges inside the snapshot. Linking a new root to
  // an original neighbour would overwrite that neighbour's reciprocal edge.
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> copies;
  std::vector<zigzag::CellRef> pending;
  const auto copy = [&](const zigzag::CellRef source) {
    if (const auto found = copies.find(source); found != copies.end()) {
      return found->second;
    }
    this->arena().consumeWork();
    const auto authority =
        arena.cloneMaster(source, core_->dims().clone).value_or(source);
    const auto spans = arena.contentOf(authority);
    std::vector<xanadu::PrimediaSpan> content(spans.begin(), spans.end());
    std::optional<std::uint32_t> quoteSpace;
    if (const auto quote = arena.quotedContent(authority)) {
      for (std::uint32_t space = 1; space <= arena.spaceCount(); ++space) {
        if (const auto attached = arena.spaceAt(space);
            attached && attached->store == quote->store) {
          quoteSpace = space;
          content    = quote->spans;
          break;
        }
      }
    }
    const auto value = core_->render(source);
    const auto snapshot =
        quoteSpace ? arena.makeQuote(*quoteSpace, content)
                   : (value ? cellFromValue(*value) : arena.makeCell());
    if (quoteSpace) {
      if (value && std::holds_alternative<std::int64_t>(*value)) {
        const auto integer = std::get<std::int64_t>(*value);
        zigzag::expectWritten(
            arena.setValueBits(snapshot, xanadu::ValueKind::Int64,
                               std::bit_cast<std::uint64_t>(integer)));
      } else if (value && std::holds_alternative<double>(*value)) {
        const auto number = std::get<double>(*value);
        zigzag::expectWritten(
            arena.setValueBits(snapshot, xanadu::ValueKind::Double,
                               std::bit_cast<std::uint64_t>(number)));
      } else if (value && std::holds_alternative<bool>(*value)) {
        const auto boolean = std::get<bool>(*value);
        zigzag::expectWritten(
            arena.setValueBits(snapshot, xanadu::ValueKind::Bool,
                               static_cast<std::uint64_t>(boolean)));
      }
    } else if (!content.empty()) {
      zigzag::expectWritten(arena.setContent(snapshot, content));
    }
    copies.emplace(source, snapshot);
    pending.push_back(source);
    return snapshot;
  };
  std::vector<zigzag::CellRef> result;
  for (const auto subject : subjects) {
    result.push_back(copy(subject));
  }
  for (std::size_t index = 0; index < pending.size(); ++index) {
    const auto source   = pending[index];
    const auto snapshot = copies.at(source);
    for (const auto &[dim, direction] : selected) {
      this->arena().consumeWork();
      if (const auto target = arena.linkedMinting(source, dim, direction)) {
        const auto copiedTarget = copy(*target);
        zigzag::expectWritten(
            arena.link(snapshot, dim, direction, copiedTarget));
      }
    }
  }
  return result;
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
    const auto &cmp  = std::get<ComparisonExpr>(factor.test);
    const auto left  = cellsOfValueExpr(*cmp.left, context);
    const auto right = cellsOfValueExpr(*cmp.right, context);
    for (const auto lhs : left) {
      for (const auto rhs : right) {
        this->arena().consumeWork();
        const auto leftValue  = core_->render(lhs);
        const auto rightValue = core_->render(rhs);
        if (leftValue && rightValue &&
            compareValues(*leftValue, cmp.op, *rightValue)) {
          result = true;
          break;
        }
      }
      if (result) {
        break;
      }
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
    } else if (const auto *fn =
                   std::get_if<std::shared_ptr<FunctionInvocation>>(
                       &test.kind)) {
      if (const auto value =
              evaluateValueExpr(ValueExpr{.kind = *fn}, context)) {
        result = zigzag::vortex::VortexCore::evaluateTruthiness(*value);
      }

    } else if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(
                   test.kind)) {
      result = evaluateShapePredicate(
          *std::get<std::shared_ptr<ShapeLiteral>>(test.kind), context);
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
          if (cur.empty()) {
            break;
          }
          this->arena().consumeWork();
          cur = evaluateStep(step, cur);
        }
        accumulatedResults.push_back(resultCell);
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

bool VQLEngine::evaluateShapePredicate(const ShapeLiteral &shape,
                                       std::optional<zigzag::CellRef> context) {
  if (!context || !core_->arena().contains(*context)) {
    return false;
  }
  for (const auto &shapeRule : shape.rules) {
    this->arena().consumeWork();
    if (std::holds_alternative<ShapeWildcard>(shapeRule.rule)) {
      auto wc = std::get<ShapeWildcard>(shapeRule.rule);
      if (wc == ShapeWildcard::Nothing) {
        return false;
      }
      continue;
    }
    if (std::holds_alternative<DimensionShapeRule>(shapeRule.rule)) {
      const auto &dr     = std::get<DimensionShapeRule>(shapeRule.rule);
      zigzag::DimRef dim = resolveDimension(dr.dimName);
      const auto linkedPos =
          core_->arena().linkedMinting(*context, dim, zigzag::DimVector::POS);
      const auto linkedNeg =
          core_->arena().linkedMinting(*context, dim, zigzag::DimVector::NEG);
      auto linked =
          (dr.direction == zigzag::DimVector::POS) ? linkedPos : linkedNeg;
      if (dr.isBidir) {
        linked = linkedPos ? linkedPos : linkedNeg;
      }

      if (dr.negated) {
        if (linked) {
          return false;
        }
      } else {
        if (!linked) {
          return false;
        }
        if (dr.targetValue) {
          const auto expectedVal = evaluateValueExpr(*dr.targetValue, context);
          const auto actualVal   = core_->render(*linked);
          if (!expectedVal || !actualVal ||
              !compareValues(*actualVal, CompOp::Equal, *expectedVal)) {
            return false;
          }
        }
      }
    } else if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(
                   shapeRule.rule)) {
      if (!evaluateShapePredicate(
              *std::get<std::shared_ptr<ShapeLiteral>>(shapeRule.rule),
              context)) {
        return false;
      }
    } else {
      throw std::runtime_error("unsupported shape predicate rule");
    }
  }
  return true;
}

} // namespace xanadu::vql

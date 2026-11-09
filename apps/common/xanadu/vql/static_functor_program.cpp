#include "common/xanadu/vql/static_functor_program.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"

#include <algorithm>
#include <bit>
#include <concepts>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace xanadu::vql {
namespace {

struct ProgramFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct UnsupportedProgram {};

class ProgramEncoder {
public:
  explicit ProgramEncoder(const StaticFunctorProgramLimits limits)
      : limits_(limits) {
    if (limits_.maxNodes == 0 || limits_.maxDepth == 0 || limits_.maxWork == 0)
      throw ProgramFailure("static functor program budget is zero");
  }

  void encode(std::span<const PathStep> steps) {
    if (steps.empty()) throw UnsupportedProgram{};
    token('P');
    number(steps.size());
    for (const auto &step : steps) pathStep(step, 1);
  }

  [[nodiscard]] std::string takeIdentity() && { return std::move(identity_); }

  [[nodiscard]] std::vector<StaticFunctorAliasUse> takeAliases() && {
    std::ranges::sort(aliases_, [](const auto &left, const auto &right) {
      if (left.name != right.name) return left.name < right.name;
      return left.role < right.role;
    });
    aliases_.erase(std::unique(aliases_.begin(), aliases_.end()),
                   aliases_.end());
    return std::move(aliases_);
  }

  [[nodiscard]] std::size_t work() const noexcept { return work_; }

  void chargeCopy(const std::size_t stepCount) {
    charge(stepCount);
    charge(identity_.size());
    for (const auto &alias : aliases_) charge(alias.name.size());
  }

private:
  void charge(const std::size_t amount = 1) {
    if (amount > limits_.maxWork - work_)
      throw ProgramFailure("static functor program work budget exhausted");
    work_ += amount;
  }

  void tick(const std::size_t depth) {
    if (depth > limits_.maxDepth)
      throw ProgramFailure("static functor program AST depth budget exhausted");
    if (nodes_ >= limits_.maxNodes)
      throw ProgramFailure("static functor program AST node budget exhausted");
    ++nodes_;
    charge();
  }

  void token(const char value) {
    charge();
    identity_.push_back(value);
  }

  void raw(const std::string_view value) {
    charge(value.size());
    identity_.append(value);
  }

  void number(const std::uint64_t value) {
    token('N');
    raw(std::to_string(value));
    token(';');
  }

  void signedNumber(const std::int64_t value) {
    token('I');
    raw(std::to_string(value));
    token(';');
  }

  void boolean(const bool value) { token(value ? '1' : '0'); }

  void string(const std::string_view value) {
    token('S');
    number(value.size());
    raw(value);
  }

  void alias(const std::string_view name,
             const StaticFunctorAliasUse::Role role) {
    if (name.empty()) throw ProgramFailure("static functor has an empty alias");
    charge();
    charge(name.size());
    aliases_.push_back({.name = std::string(name), .role = role});
  }

  template <class Pointer, class Action>
  void pointer(const Pointer &value, const std::size_t depth,
               const std::string_view name, Action action) {
    if (!value)
      throw ProgramFailure("static functor has a null " + std::string(name));
    const void *identity = value.get();
    if (!active_.insert(identity).second)
      throw ProgramFailure("static functor program contains a cycle");
    struct EraseOnExit {
      std::unordered_set<const void *> &active;
      const void *identity;
      ~EraseOnExit() { active.erase(identity); }
    } erase{active_, identity};
    action(*value, depth);
  }

  void literal(const ScalarLiteral &value) {
    token('L');
    std::visit(
        [this](const auto &item) {
          using T = std::remove_cvref_t<decltype(item)>;
          if constexpr (std::same_as<T, std::string>) {
            token('s');
            string(item);
          } else if constexpr (std::same_as<T, double>) {
            token('d');
            number(std::bit_cast<std::uint64_t>(item));
          } else if constexpr (std::same_as<T, std::int64_t>) {
            token('i');
            signedNumber(item);
          } else {
            token('b');
            boolean(item);
          }
        },
        value.value);
  }

  void value(const ValueExpr &expr, const std::size_t depth) {
    tick(depth);
    std::visit(
        [this, depth](const auto &part) {
          using Part = std::remove_cvref_t<decltype(part)>;
          if constexpr (std::same_as<Part, ScalarLiteral>) {
            literal(part);
          } else if constexpr (std::same_as<Part, std::string>) {
            token('V');
            string(part);
            alias(part, StaticFunctorAliasUse::Role::Value);
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<PathExpression>>) {
            token('X');
            pointer(part, depth + 1, "value path",
                    [this](const PathExpression &path,
                           const std::size_t childDepth) {
                      pathExpression(path, childDepth);
                    });
          } else if constexpr (std::same_as<
                                   Part, std::shared_ptr<FunctionInvocation>>) {
            token('F');
            pointer(part, depth + 1, "function call",
                    [this](const FunctionInvocation &call,
                           const std::size_t childDepth) {
                      invocation(call, childDepth);
                    });
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<BooleanExpr>>) {
            token('B');
            pointer(part, depth + 1, "Boolean expression",
                    [this](const BooleanExpr &booleanExpr,
                           const std::size_t childDepth) {
                      booleanExpression(booleanExpr, childDepth);
                    });
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<BinaryValueExpr>>) {
            token('A');
            pointer(
                part, depth + 1, "binary expression",
                [this](const BinaryValueExpr &binary,
                       const std::size_t childDepth) {
                  if (!binary.left || !binary.right)
                    throw ProgramFailure(
                        "static functor has an incomplete binary expression");
                  pointer(binary.left, childDepth + 1, "binary left value",
                          [this](const ValueExpr &left,
                                 const std::size_t nestedDepth) {
                            value(left, nestedDepth);
                          });
                  pointer(binary.right, childDepth + 1, "binary right value",
                          [this](const ValueExpr &right,
                                 const std::size_t nestedDepth) {
                            value(right, nestedDepth);
                          });
                });
          } else {
            throw UnsupportedProgram{};
          }
        },
        expr.kind);
  }

  void pathExpression(const PathExpression &path, const std::size_t depth) {
    tick(depth);
    token('X');
    if (path.isFunctor || (path.anchor.kind != AnchorKind::Context &&
                           path.anchor.kind != AnchorKind::Variable))
      throw UnsupportedProgram{};
    boolean(path.isFunctor);
    token(static_cast<char>(path.anchor.kind));
    string(path.anchor.name);
    if (path.anchor.kind == AnchorKind::Variable)
      alias(path.anchor.name, StaticFunctorAliasUse::Role::Value);
    if (path.anchor.derefMaster || path.anchor.createValue ||
        path.anchor.generator || path.anchor.storeAnchor ||
        path.anchor.versionPin || path.anchor.cellId != 0 ||
        !path.anchor.microversionId.empty() || !path.anchorPredicates.empty() ||
        path.anchorProjection || path.shapeProjection || path.cloneTail)
      throw UnsupportedProgram{};
    number(path.steps.size());
    for (const auto &step : path.steps) pathStep(step, depth + 1);
  }

  void invocation(const FunctionInvocation &call, const std::size_t depth) {
    tick(depth);
    if (call.isSliceRead ||
        std::ranges::any_of(call.argNames, [](const std::string &name) {
          return !name.empty();
        }))
      throw UnsupportedProgram{};
    string(call.name);
    boolean(call.isSliceRead);
    number(call.args.size());
    for (const auto &argument : call.args) value(argument, depth + 1);
    number(call.argNames.size());
    for (const auto &name : call.argNames) string(name);
  }

  void comparison(const ComparisonExpr &expr, const std::size_t depth) {
    tick(depth);
    token('C');
    number(static_cast<std::uint64_t>(expr.op));
    if (!expr.left || !expr.right)
      throw ProgramFailure("static functor has a null comparison operand");
    pointer(expr.left, depth + 1, "comparison left value",
            [this](const ValueExpr &left, const std::size_t childDepth) {
              value(left, childDepth);
            });
    pointer(expr.right, depth + 1, "comparison right value",
            [this](const ValueExpr &right, const std::size_t childDepth) {
              value(right, childDepth);
            });
  }

  void predicateTest(const PredicateTest &test, const std::size_t depth) {
    tick(depth);
    std::visit(
        [this, depth](const auto &part) {
          using Part = std::remove_cvref_t<decltype(part)>;
          if constexpr (std::same_as<Part, bool>) {
            token('b');
            boolean(part);
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<PathExpression>>) {
            token('x');
            pointer(part, depth + 1, "predicate path",
                    [this](const PathExpression &path,
                           const std::size_t childDepth) {
                      pathExpression(path, childDepth);
                    });
          } else if constexpr (std::same_as<Part, std::shared_ptr<ValueExpr>>) {
            token('v');
            pointer(
                part, depth + 1, "predicate value",
                [this](const ValueExpr &expr, const std::size_t childDepth) {
                  value(expr, childDepth);
                });
          } else if constexpr (std::same_as<
                                   Part, std::shared_ptr<FunctionInvocation>>) {
            token('f');
            pointer(part, depth + 1, "predicate call",
                    [this](const FunctionInvocation &call,
                           const std::size_t childDepth) {
                      invocation(call, childDepth);
                    });
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<ShapeLiteral>>) {
            token('s');
            pointer(part, depth + 1, "predicate shape",
                    [this](const ShapeLiteral &shape,
                           const std::size_t childDepth) {
                      shapeLiteral(shape, childDepth);
                    });
          }
        },
        test.kind);
  }

  void booleanExpression(const BooleanExpr &expr, const std::size_t depth) {
    tick(depth);
    if (expr.terms.empty())
      throw ProgramFailure("static functor has an empty Boolean expression");
    token('O');
    number(expr.terms.size());
    for (const auto &term : expr.terms) {
      tick(depth + 1);
      if (term.factors.empty())
        throw ProgramFailure("static functor has an empty Boolean term");
      token('&');
      number(term.factors.size());
      for (const auto &factor : term.factors) {
        tick(depth + 2);
        token('!');
        boolean(factor.negated);
        std::visit(
            [this, depth](const auto &test) {
              using Test = std::remove_cvref_t<decltype(test)>;
              if constexpr (std::same_as<Test, ComparisonExpr>) {
                comparison(test, depth + 3);
              } else {
                predicateTest(test, depth + 3);
              }
            },
            factor.test);
      }
    }
  }

  void shapeLiteral(const ShapeLiteral &shape, const std::size_t depth) {
    tick(depth);
    if (shape.rules.empty()) throw UnsupportedProgram{};
    token('S');
    number(shape.rules.size());
    for (const auto &entry : shape.rules) {
      tick(depth + 1);
      const auto *rule = std::get_if<DimensionShapeRule>(&entry.rule);
      if (!rule || !rule->dimName.starts_with("d.") ||
          rule->dimName.size() <= 2)
        throw UnsupportedProgram{};
      token('D');
      boolean(rule->negated);
      boolean(rule->isBidir);
      signedNumber(static_cast<std::int64_t>(rule->direction));
      string(rule->dimName);
      boolean(rule->targetValue != nullptr);
      if (!rule->targetValue) continue;
      pointer(
          rule->targetValue, depth + 2, "shape target",
          [this](const ValueExpr &target, const std::size_t childDepth) {
            if (const auto *literal =
                    std::get_if<ScalarLiteral>(&target.kind)) {
              token('l');
              this->literal(*literal);
              return;
            }
            if (const auto *nested =
                    std::get_if<std::shared_ptr<ShapeLiteral>>(&target.kind);
                nested && *nested) {
              token('s');
              pointer(*nested, childDepth + 1, "nested shape target",
                      [this](const ShapeLiteral &shape,
                             const std::size_t nestedDepth) {
                        shapeLiteral(shape, nestedDepth);
                      });
              return;
            }
            throw UnsupportedProgram{};
          });
    }
  }

  void dimensionStep(const SignedDimensionStep &dimension,
                     const std::size_t depth) {
    tick(depth);
    token('d');
    string(dimension.dimName);
    if (dimension.dimName.starts_with('$'))
      alias(std::string_view(dimension.dimName).substr(1),
            StaticFunctorAliasUse::Role::Functor);
    signedNumber(static_cast<std::int64_t>(dimension.direction));
    boolean(dimension.isBidir);
    number(static_cast<std::uint64_t>(dimension.placement));
    if (!dimension.creates.empty()) throw UnsupportedProgram{};
  }

  bool capturedSelectorIsCanonical(const PathStep &step) const {
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    return dimension && dimension->dimName.starts_with('$') &&
           dimension->direction == zigzag::DimVector::POS &&
           !dimension->isBidir && dimension->placement == Placement::Default &&
           dimension->creates.empty();
  }

  void pathStep(const PathStep &step, const std::size_t depth) {
    tick(depth);
    token('p');
    if (step.capturedFunctor) {
      if (!capturedSelectorIsCanonical(step)) throw UnsupportedProgram{};
      token('c');
      pointer(step.capturedFunctor, depth + 1, "captured path body",
              [this](const std::vector<PathStep> &body,
                     const std::size_t childDepth) {
                number(body.size());
                for (const auto &nested : body)
                  pathStep(nested, childDepth + 1);
              });
    } else if (const auto *dimension =
                   std::get_if<SignedDimensionStep>(&step.selector)) {
      dimensionStep(*dimension, depth + 1);
    } else if (const auto *call =
                   std::get_if<FunctionInvocation>(&step.selector)) {
      token('f');
      invocation(*call, depth + 1);
    } else if (const auto *group =
                   std::get_if<std::shared_ptr<MacroDimensionGroup>>(
                       &step.selector)) {
      token('m');
      pointer(*group, depth + 1, "macro group",
              [this](const MacroDimensionGroup &macro,
                     const std::size_t childDepth) {
                number(static_cast<std::uint64_t>(macro.repetition));
                number(macro.steps.size());
                for (const auto &nested : macro.steps)
                  pathStep(nested, childDepth + 1);
              });
    } else {
      throw UnsupportedProgram{};
    }

    boolean(step.derefMaster);
    number(step.predicates.size());
    for (const auto &predicate : step.predicates)
      booleanExpression(predicate, depth + 1);
    number(step.shapePredicates.size());
    for (const auto &shape : step.shapePredicates)
      shapeLiteral(shape, depth + 1);
    if (step.projectionAfter || step.versionPin) throw UnsupportedProgram{};
    boolean(step.rangeClamp.has_value());
    if (step.rangeClamp) {
      signedNumber(step.rangeClamp->start);
      boolean(step.rangeClamp->end.has_value());
      if (step.rangeClamp->end) signedNumber(*step.rangeClamp->end);
    }
    number(static_cast<std::uint64_t>(step.yieldMode));
  }

  StaticFunctorProgramLimits limits_;
  std::size_t nodes_{0};
  std::size_t work_{0};
  std::string identity_;
  std::vector<StaticFunctorAliasUse> aliases_;
  std::unordered_set<const void *> active_;
};

} // namespace

std::expected<std::optional<StaticFunctorProgram>, StaticFunctorProgramError>
normalizeStaticFunctorProgram(const std::span<const PathStep> steps,
                              const std::size_t formalIndex,
                              const StaticFunctorProgramLimits limits) {
  try {
    const auto scope =
        analyzeFrameStaticFunctorScope(steps, {.maxNodes = limits.maxNodes,
                                               .maxDepth = limits.maxDepth,
                                               .maxWork  = limits.maxWork});
    if (!scope)
      return std::unexpected(StaticFunctorProgramError{scope.error().message});
    if (scope->refusalReason) return std::optional<StaticFunctorProgram>{};

    if (scope->workUnits >= limits.maxWork)
      return std::unexpected(StaticFunctorProgramError{
          "static functor program work budget exhausted"});
    auto remainingLimits = limits;
    remainingLimits.maxWork -= scope->workUnits;
    ProgramEncoder encoder(remainingLimits);
    encoder.encode(steps);
    encoder.chargeCopy(steps.size());
    auto identity          = std::move(encoder).takeIdentity();
    auto aliases           = std::move(encoder).takeAliases();
    const std::size_t work = scope->workUnits + encoder.work();
    auto owned = std::make_shared<const std::vector<PathStep>>(steps.begin(),
                                                               steps.end());
    return std::optional<StaticFunctorProgram>{
        StaticFunctorProgram{.key       = {.formalIndex        = formalIndex,
                                           .structuralIdentity = std::move(identity)},
                             .steps     = std::move(owned),
                             .aliases   = std::move(aliases),
                             .workUnits = work}};
  } catch (const UnsupportedProgram &) {
    return std::optional<StaticFunctorProgram>{};
  } catch (const std::exception &error) {
    return std::unexpected(StaticFunctorProgramError{error.what()});
  }
}

} // namespace xanadu::vql

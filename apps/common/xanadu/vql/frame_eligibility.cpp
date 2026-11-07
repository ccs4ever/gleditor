#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/projection_syntax.hpp"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace xanadu::vql {
namespace {

using Scope = std::unordered_set<std::string>;

struct CallSite {
  std::string callee;
  Scope visibleNames;
  bool inParameterDefault{false};
  bool conditionalOnTypedRest{false};
  bool mayInjectContext{false};
  std::size_t argumentCount{0};
};

struct CaptureDependency {
  std::string caller;
  Scope visibleNames;
  bool inParameterDefault{false};
  bool conditionalOnTypedRest{false};
  bool mayInjectContext{false};
  std::size_t argumentCount{0};
};

enum class CaptureRequirement : std::uint8_t {
  Free,
  Required,
  ConditionalRequired,
};

struct PendingCapture {
  std::string definition;
  std::string name;
  CaptureRequirement requirement{CaptureRequirement::Free};
};

struct ScanFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct TraversalBudget {
  explicit TraversalBudget(const FrameEligibilityLimits configured)
      : limits(configured) {
    if (limits.maxNodes == 0 || limits.maxDepth == 0)
      throw ScanFailure("frame eligibility traversal budget is zero");
  }

  void tick(const std::size_t depth) {
    if (depth > limits.maxDepth)
      throw ScanFailure("frame eligibility AST depth budget exhausted");
    charge();
  }

  void charge() {
    if (nodes >= limits.maxNodes)
      throw ScanFailure("frame eligibility AST node budget exhausted");
    ++nodes;
  }

  FrameEligibilityLimits limits;
  std::size_t nodes{0};
  std::unordered_set<const void *> activePointers;
};

class DefinitionScanner {
public:
  explicit DefinitionScanner(TraversalBudget &budget) : budget_(budget) {}

  std::optional<std::string> scan(const DefDeclaration &definition) {
    tick(0);
    if (definition.effect == DefDeclaration::Effect::Weave)
      refuse("weave definitions are not reusable frame candidates");

    Scope scope;
    bool contextParameterSeen = false;
    for (std::size_t index = 0; index < definition.params.size(); ++index) {
      const auto &parameter = definition.params[index];
      tick(1);
      if (parameter.isVariadic &&
          (index + 1 != definition.params.size() || parameter.defaultValue ||
           !std::holds_alternative<std::string>(parameter.target)))
        refuse(
            "only named final variadics without defaults are frame eligible");
      if (parameter.defaultValue) {
        const bool prior          = scanningParameterDefault_;
        scanningParameterDefault_ = true;
        value(*parameter.defaultValue, scope, 2);
        scanningParameterDefault_ = prior;
      }
      if (parameter.type) {
        const Scope priorScope       = scope;
        const bool savedConditional  = scanningConditionalRestType_;
        scanningConditionalRestType_ = parameter.isVariadic;
        auto captures                = type(*parameter.type, priorScope, 2);
        scanningConditionalRestType_ = savedConditional;
        if (!parameter.isVariadic)
          scope.insert(captures.begin(), captures.end());
      }

      if (const auto *name = std::get_if<std::string>(&parameter.target)) {
        if (name->empty() || !scope.insert(*name).second)
          refuse("frame parameters must have unique variable names");
      } else if (std::holds_alternative<bool>(parameter.target)) {
        if (contextParameterSeen)
          refuse("multiple context parameters remain on inline graph lowering");
        contextParameterSeen = true;
      } else {
        const auto &pattern = std::get<DestructurePattern>(parameter.target);
        auto captures       = contractPattern(pattern, scope, 2);
        scope.insert(captures.begin(), captures.end());
      }
    }

    Scope returnScope = scope;
    if (const auto *body =
            std::get_if<std::shared_ptr<ValueExpr>>(&definition.body)) {
      pointer(*body, 1, [&](const ValueExpr &expr, const std::size_t depth) {
        value(expr, scope, depth);
      });
    } else {
      const auto &scriptRoot =
          std::get<std::shared_ptr<ScriptBody>>(definition.body);
      pointer(scriptRoot, 1,
              [&](const ScriptBody &script, const std::size_t depth) {
                returnScope = scriptBody(script, scope, depth);
              });
    }
    if (definition.returnType)
      (void)type(*definition.returnType, returnScope, 1);
    return refusal_;
  }

  [[nodiscard]] const std::unordered_set<std::string> &
  freeCaptureNames() const noexcept {
    return freeCaptureNames_;
  }

  [[nodiscard]] const std::unordered_set<std::string> &
  requiredDefaultCaptureBindings() const noexcept {
    return requiredDefaultCaptureBindings_;
  }

  [[nodiscard]] const std::unordered_set<std::string> &
  conditionalRequiredDefaultCaptureBindings() const noexcept {
    return conditionalRequiredDefaultCaptureBindings_;
  }

  [[nodiscard]] std::vector<CallSite> takeCallSites() && {
    return std::move(callSites_);
  }

  void validateQueryNodes(const QueryExpression &query) {
    Scope openScope;
    scriptBody(query.body, openScope, 0);
    std::visit(
        [&](const auto &expression) {
          using Expression = std::remove_cvref_t<decltype(expression)>;
          if constexpr (std::same_as<Expression, ExecutionBlock>)
            executionBlock(expression, openScope, 1);
          else
            pathExpression(expression, openScope, 1);
        },
        query.expr);
    sequence(query.mounts, 1,
             [&](const MountDeclaration &mount, const std::size_t depth) {
               storeAnchor(mount.storeAnchor, openScope, depth);
             });
    sequence(
        query.namespaceDeclarations, 1,
        [&](const NamespaceDeclaration &declaration, const std::size_t depth) {
          std::visit(
              [&](const auto &part) {
                using Part = std::remove_cvref_t<decltype(part)>;
                if constexpr (std::same_as<Part, MountDeclaration>)
                  storeAnchor(part.storeAnchor, openScope, depth + 1);
              },
              declaration);
        });
  }

private:
  template <class T, class Action>
  void pointer(const std::shared_ptr<T> &node, const std::size_t depth,
               Action action) {
    if (!node) throw ScanFailure("frame eligibility found a null AST node");
    const void *identity = node.get();
    if (!budget_.activePointers.insert(identity).second)
      throw ScanFailure("frame eligibility found a cyclic AST");
    struct EraseOnExit {
      std::unordered_set<const void *> &set;
      const void *identity;
      ~EraseOnExit() { set.erase(identity); }
    } erase{budget_.activePointers, identity};
    tick(depth);
    action(*node, depth + 1);
  }

  void tick(const std::size_t depth) { budget_.tick(depth); }

  void refuse(std::string message) {
    if (!refusal_) refusal_ = std::move(message);
  }

  void variable(const std::string &name, const Scope &scope,
                const bool requiredDefaultBinding = false) {
    tick(0);
    if (name.empty()) {
      refuse("frame eligibility found an empty lexical variable");
      return;
    }
    if (!scope.contains(name)) {
      freeCaptureNames_.insert(name);
      if (requiredDefaultBinding) {
        if (scanningConditionalRestType_)
          conditionalRequiredDefaultCaptureBindings_.insert(name);
        else
          requiredDefaultCaptureBindings_.insert(name);
      }
    }
  }

  template <class T, class Action>
  void sequence(const T &items, const std::size_t depth, Action action) {
    for (const auto &item : items) {
      tick(depth);
      action(item, depth + 1);
    }
  }

  void value(const ValueExpr &expr, const Scope &scope,
             const std::size_t depth) {
    tick(depth);
    std::visit(
        [&](const auto &part) {
          using Part = std::remove_cvref_t<decltype(part)>;
          if constexpr (std::same_as<Part, ScalarLiteral>) {
            return;
          } else if constexpr (std::same_as<Part, std::string>) {
            variable(part, scope);
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<PathExpression>>) {
            pointer(
                part, depth + 1,
                [&](const PathExpression &path, const std::size_t childDepth) {
                  pathExpression(path, scope, childDepth);
                });
          } else if constexpr (std::same_as<
                                   Part, std::shared_ptr<FunctionInvocation>>) {
            pointer(part, depth + 1,
                    [&](const FunctionInvocation &call,
                        const std::size_t childDepth) {
                      invocation(call, scope, childDepth);
                    });
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<ShapeLiteral>>) {
            pointer(
                part, depth + 1,
                [&](const ShapeLiteral &shape, const std::size_t childDepth) {
                  shapeLiteral(shape, scope, childDepth);
                });
            refuse("first-class shape values are not frame eligible");
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<VersionExpr>>) {
            pointer(
                part, depth + 1,
                [&](const VersionExpr &version, const std::size_t childDepth) {
                  versionExpr(version, scope, childDepth);
                });
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<PathLambda>>) {
            pointer(
                part, depth + 1,
                [&](const PathLambda &lambda, const std::size_t childDepth) {
                  refuse("path lambdas are not reusable closed frame bodies");
                  pathLambda(lambda, scope, childDepth);
                });
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<BooleanExpr>>) {
            pointer(
                part, depth + 1,
                [&](const BooleanExpr &boolean, const std::size_t childDepth) {
                  booleanExpr(boolean, scope, childDepth);
                });
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<BinaryValueExpr>>) {
            pointer(part, depth + 1,
                    [&](const BinaryValueExpr &binary,
                        const std::size_t childDepth) {
                      pointer(binary.left, childDepth + 1,
                              [&](const ValueExpr &left,
                                  const std::size_t nestedDepth) {
                                value(left, scope, nestedDepth);
                              });
                      pointer(binary.right, childDepth + 1,
                              [&](const ValueExpr &right,
                                  const std::size_t nestedDepth) {
                                value(right, scope, nestedDepth);
                              });
                    });
          }
        },
        expr.kind);
  }

  void invocation(const FunctionInvocation &call, const Scope &scope,
                  const std::size_t depth,
                  const bool mayInjectContext = false) {
    tick(depth);
    Scope visibleNames;
    for (const auto &name : scope) {
      budget_.charge();
      visibleNames.insert(name);
    }
    callSites_.push_back(
        {.callee                 = call.name,
         .visibleNames           = std::move(visibleNames),
         .inParameterDefault     = scanningParameterDefault_,
         .conditionalOnTypedRest = scanningConditionalRestType_,
         .mayInjectContext       = mayInjectContext,
         .argumentCount          = call.args.size()});
    if (call.isSliceRead) refuse("slice reads are not reusable frame calls");
    if (std::ranges::any_of(call.argNames, [](const std::string &name) {
          return !name.empty();
        }))
      refuse("named function arguments are not reusable frame calls");
    sequence(call.args, depth + 1,
             [&](const ValueExpr &argument, const std::size_t childDepth) {
               value(argument, scope, childDepth);
             });
  }

  void versionExpr(const VersionExpr &expr, const Scope &scope,
                   const std::size_t depth) {
    tick(depth);
    if (expr.op == VersionExpr::Op::Variable) variable(expr.versionStr, scope);
    if (expr.left)
      pointer(expr.left, depth + 1,
              [&](const VersionExpr &left, const std::size_t childDepth) {
                versionExpr(left, scope, childDepth);
              });
    if (expr.right)
      pointer(expr.right, depth + 1,
              [&](const VersionExpr &right, const std::size_t childDepth) {
                versionExpr(right, scope, childDepth);
              });
  }

  void versionPin(const VersionPin &pin, const Scope &scope,
                  const std::size_t depth) {
    sequence(pin.versions, depth,
             [&](const VersionExpr &version, const std::size_t childDepth) {
               versionExpr(version, scope, childDepth);
             });
    refuse("historical version pins are not frame eligible");
  }

  void createValue(const CreateValue &create, const Scope &scope,
                   const std::size_t depth) {
    tick(depth);
    if (create.kind == CreateValue::Kind::Expression) {
      if (!create.expr) throw ScanFailure("create expression is missing");
      pointer(create.expr, depth + 1,
              [&](const ValueExpr &expr, const std::size_t childDepth) {
                value(expr, scope, childDepth);
              });
    }
    refuse("creation expressions are not reusable frame bodies");
  }

  void storeAnchor(const StoreAnchor &anchor, const Scope &scope,
                   const std::size_t depth) {
    sequence(anchor.args, depth,
             [&](const ValueExpr &arg, const std::size_t childDepth) {
               value(arg, scope, childDepth);
             });
    if (!anchor.sliceName.empty())
      refuse("store slice anchors are not reusable frame bodies");
  }

  void projectionShape(const ShapeLiteral &shape, const Scope &scope,
                       const std::size_t depth) {
    shapeLiteral(shape, scope, depth, true);
    if (!isStaticShapeProjectionSyntax(shape))
      refuse("shape projection is outside the native frame subset");
    for (const ShapeRule &entry : shape.rules) {
      const auto *rule = std::get_if<DimensionShapeRule>(&entry.rule);
      if (rule && rule->targetValue && rule->dimName == "d.clone" &&
          !std::holds_alternative<ScalarLiteral>(rule->targetValue->kind))
        refuse("nonliteral clone-dimension projection is not frame eligible");
    }
  }

  void pathExpression(const PathExpression &path, const Scope &scope,
                      const std::size_t depth) {
    tick(depth);
    if (path.isFunctor)
      refuse("partial path functors are not reusable frame bodies");
    if (path.anchor.kind == AnchorKind::Variable)
      variable(path.anchor.name, scope);
    if (path.anchor.kind == AnchorKind::NamedStore ||
        path.anchor.kind == AnchorKind::Cursor ||
        path.anchor.kind == AnchorKind::NamedCursor ||
        path.anchor.kind == AnchorKind::AllCursors ||
        path.anchor.kind == AnchorKind::AllNamedCursors ||
        path.anchor.kind == AnchorKind::StoreGenerator ||
        path.anchor.kind == AnchorKind::StoreAnchor ||
        path.anchor.kind == AnchorKind::Microversion)
      refuse("dynamic or historical anchors are not frame eligible");
    if (path.anchor.kind == AnchorKind::Create)
      refuse("creation anchors are not reusable frame bodies");
    if (path.anchor.createValue)
      createValue(*path.anchor.createValue, scope, depth + 1);
    if (path.anchor.generator) {
      sequence(path.anchor.generator->args, depth + 1,
               [&](const ValueExpr &arg, const std::size_t childDepth) {
                 value(arg, scope, childDepth);
               });
      refuse("store generators are not reusable frame bodies");
    }
    if (path.anchor.storeAnchor) {
      storeAnchor(*path.anchor.storeAnchor, scope, depth + 1);
      refuse("store anchors are not reusable frame bodies");
    }
    if (path.anchor.versionPin)
      versionPin(*path.anchor.versionPin, scope, depth + 1);
    sequence(path.anchorPredicates, depth + 1,
             [&](const BooleanExpr &predicate, const std::size_t childDepth) {
               booleanExpr(predicate, scope, childDepth);
             });
    if (path.anchorProjection)
      projectionShape(*path.anchorProjection, scope, depth + 1);
    sequence(path.steps, depth + 1,
             [&](const PathStep &step, const std::size_t childDepth) {
               pathStep(step, scope, childDepth);
             });
    if (path.shapeProjection)
      projectionShape(*path.shapeProjection, scope, depth + 1);
    if (path.cloneTail) {
      refuse("clone tails are not reusable frame bodies");
      sequence(path.cloneTail->operands, depth + 1,
               [&](const CloneOperand &operand, const std::size_t childDepth) {
                 if (operand.path)
                   pointer(operand.path, childDepth,
                           [&](const PathExpression &nested,
                               const std::size_t nestedDepth) {
                             pathExpression(nested, scope, nestedDepth);
                           });
                 if (operand.bareCreate)
                   createValue(*operand.bareCreate, scope, childDepth);
               });
    }
  }

  void signedStep(const SignedDimensionStep &step, const Scope &scope,
                  const std::size_t depth) {
    if (step.dimName.starts_with('$')) {
      variable(step.dimName.substr(1), scope);
      refuse("path-functor applications remain on inline graph lowering");
    }
    sequence(step.creates, depth,
             [&](const CreateValue &create, const std::size_t childDepth) {
               createValue(create, scope, childDepth);
             });
  }

  void pathStep(const PathStep &step, const Scope &scope,
                const std::size_t depth) {
    tick(depth);
    std::visit(
        [&](const auto &selector) {
          using Selector = std::remove_cvref_t<decltype(selector)>;
          if constexpr (std::same_as<Selector, SignedDimensionStep>) {
            signedStep(selector, scope, depth + 1);
          } else if constexpr (std::same_as<
                                   Selector,
                                   std::shared_ptr<MacroDimensionGroup>>) {
            pointer(selector, depth + 1,
                    [&](const MacroDimensionGroup &macro,
                        const std::size_t childDepth) {
                      sequence(macro.steps, childDepth,
                               [&](const PathStep &nested,
                                   const std::size_t nestedDepth) {
                                 pathStep(nested, scope, nestedDepth);
                               });
                    });
          } else if constexpr (std::same_as<Selector, FunctionInvocation>) {
            invocation(selector, scope, depth + 1, true);
          } else if constexpr (std::same_as<Selector, GenesisSelector> ||
                               std::same_as<Selector, DimensionPivotStep> ||
                               std::same_as<Selector, RootSelectorStep>) {
            return;
          } else if constexpr (std::same_as<Selector, SliceSelectorStep> ||
                               std::same_as<Selector, DocSelectorStep>) {
            if (selector.predicateFilter)
              pointer(selector.predicateFilter, depth + 1,
                      [&](const BooleanExpr &predicate,
                          const std::size_t childDepth) {
                        booleanExpr(predicate, scope, childDepth);
                      });
            refuse("document and slice selectors are not frame eligible");
          } else if constexpr (std::same_as<Selector, PathMorphism>) {
            value(selector.subject, scope, depth + 1);
            sequence(
                selector.targets, depth + 1,
                [&](const ValueExpr &target, const std::size_t childDepth) {
                  value(target, scope, childDepth);
                });
            refuse("path morphisms are not frame eligible");
          } else if constexpr (std::same_as<Selector, VirtualDimensionStep>) {
            signedStep(selector.dimStep, scope, depth + 1);
            value(selector.expr, scope, depth + 1);
            refuse("virtual dimensions are not frame eligible");
          } else if constexpr (std::same_as<Selector, PerspectiveStep>) {
            if (selector.targetStep)
              pointer(
                  selector.targetStep, depth + 1,
                  [&](const PathStep &nested, const std::size_t childDepth) {
                    pathStep(nested, scope, childDepth);
                  });
            refuse("perspective selectors are not frame eligible");
          } else if constexpr (std::same_as<Selector,
                                            std::shared_ptr<PathLambda>>) {
            pointer(
                selector, depth + 1,
                [&](const PathLambda &lambda, const std::size_t childDepth) {
                  pathLambda(lambda, scope, childDepth);
                });
          }
        },
        step.selector);

    sequence(step.predicates, depth + 1,
             [&](const BooleanExpr &predicate, const std::size_t childDepth) {
               booleanExpr(predicate, scope, childDepth);
             });
    sequence(step.shapePredicates, depth + 1,
             [&](const ShapeLiteral &shape, const std::size_t childDepth) {
               shapeLiteral(shape, scope, childDepth);
             });
    if (step.versionPin) versionPin(*step.versionPin, scope, depth + 1);
    if (step.yieldMode == YieldMode::Both || step.yieldMode == YieldMode::Last)
      refuse("non-default path yield modes are not frame eligible");
    if (step.capturedFunctor) {
      refuse("captured path functors are not frame eligible");
      const auto &captured = step.capturedFunctor;
      const void *identity = captured.get();
      if (!budget_.activePointers.insert(identity).second)
        throw ScanFailure("frame eligibility found a cyclic AST");
      struct EraseOnExit {
        std::unordered_set<const void *> &set;
        const void *identity;
        ~EraseOnExit() { set.erase(identity); }
      } erase{budget_.activePointers, identity};
      sequence(*captured, depth + 1,
               [&](const PathStep &nested, const std::size_t childDepth) {
                 pathStep(nested, scope, childDepth);
               });
    }
    if (step.projectionAfter)
      projectionShape(*step.projectionAfter, scope, depth + 1);
  }

  void booleanExpr(const BooleanExpr &expr, const Scope &scope,
                   const std::size_t depth) {
    tick(depth);
    sequence(expr.terms, depth + 1,
             [&](const BooleanTerm &term, const std::size_t termDepth) {
               sequence(term.factors, termDepth,
                        [&](const BooleanFactor &factor,
                            const std::size_t factorDepth) {
                          booleanFactor(factor, scope, factorDepth);
                        });
             });
  }

  void booleanFactor(const BooleanFactor &factor, const Scope &scope,
                     const std::size_t depth) {
    tick(depth);
    std::visit(
        [&](const auto &test) {
          using Test = std::remove_cvref_t<decltype(test)>;
          if constexpr (std::same_as<Test, ComparisonExpr>) {
            if (test.left)
              pointer(test.left, depth + 1,
                      [&](const ValueExpr &valueExpr,
                          const std::size_t childDepth) {
                        value(valueExpr, scope, childDepth);
                      });
            if (test.right)
              pointer(test.right, depth + 1,
                      [&](const ValueExpr &valueExpr,
                          const std::size_t childDepth) {
                        value(valueExpr, scope, childDepth);
                      });
          } else {
            predicate(test, scope, depth + 1);
          }
        },
        factor.test);
  }

  void predicate(const PredicateTest &test, const Scope &scope,
                 const std::size_t depth) {
    std::visit(
        [&](const auto &part) {
          using Part = std::remove_cvref_t<decltype(part)>;
          if constexpr (std::same_as<Part, bool>) {
            return;
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<PathExpression>>) {
            pointer(
                part, depth,
                [&](const PathExpression &path, const std::size_t childDepth) {
                  pathExpression(path, scope, childDepth);
                });
          } else if constexpr (std::same_as<Part, std::shared_ptr<ValueExpr>>) {
            pointer(part, depth,
                    [&](const ValueExpr &expr, const std::size_t childDepth) {
                      value(expr, scope, childDepth);
                    });
          } else if constexpr (std::same_as<
                                   Part, std::shared_ptr<FunctionInvocation>>) {
            pointer(part, depth,
                    [&](const FunctionInvocation &call,
                        const std::size_t childDepth) {
                      invocation(call, scope, childDepth);
                    });
          } else if constexpr (std::same_as<Part,
                                            std::shared_ptr<ShapeLiteral>>) {
            pointer(
                part, depth,
                [&](const ShapeLiteral &shape, const std::size_t childDepth) {
                  shapeLiteral(shape, scope, childDepth);
                });
          }
        },
        test.kind);
  }

  void shapeLiteral(const ShapeLiteral &shape, const Scope &scope,
                    const std::size_t depth,
                    const bool projectionCandidate = false) {
    tick(depth);
    sequence(
        shape.rules, depth + 1,
        [&](const ShapeRule &rule, const std::size_t childDepth) {
          std::visit(
              [&](const auto &part) {
                using Part = std::remove_cvref_t<decltype(part)>;
                if constexpr (std::same_as<Part, ShapeWildcard>) {
                  return;
                } else if constexpr (std::same_as<Part, std::string>) {
                  variable(part, scope);
                } else if constexpr (std::same_as<
                                         Part,
                                         std::shared_ptr<FunctionInvocation>>) {
                  pointer(part, childDepth,
                          [&](const FunctionInvocation &call,
                              const std::size_t nestedDepth) {
                            invocation(call, scope, nestedDepth);
                          });
                } else if constexpr (std::same_as<Part, DimensionShapeRule>) {
                  if (part.targetValue)
                    pointer(part.targetValue, childDepth,
                            [&](const ValueExpr &expr,
                                const std::size_t nestedDepth) {
                              value(expr, scope, nestedDepth);
                            });
                  if (!projectionCandidate)
                    refuse("shape rules are not frame eligible");
                } else if constexpr (std::same_as<Part,
                                                  VirtualDimensionDefRule>) {
                  if (part.expr)
                    pointer(part.expr, childDepth,
                            [&](const ValueExpr &expr,
                                const std::size_t nestedDepth) {
                              value(expr, scope, nestedDepth);
                            });
                  refuse("shape rules are not frame eligible");
                } else if constexpr (std::same_as<Part, DimensionPivotRule>) {
                  refuse("shape rules are not frame eligible");
                } else if constexpr (std::same_as<
                                         Part, std::shared_ptr<ShapeLiteral>>) {
                  pointer(part, childDepth,
                          [&](const ShapeLiteral &nested,
                              const std::size_t nestedDepth) {
                            shapeLiteral(nested, scope, nestedDepth);
                          });
                }
              },
              rule.rule);
        });
  }

  void destructure(const DestructurePattern &pattern, Scope &scope,
                   const std::size_t depth) {
    sequence(
        pattern.fields, depth,
        [&](const DestructureField &field, const std::size_t childDepth) {
          if (field.defaultValue)
            pointer(field.defaultValue, childDepth,
                    [&](const ValueExpr &expr, const std::size_t nestedDepth) {
                      value(expr, scope, nestedDepth);
                    });
          if (field.type)
            pointer(
                field.type, childDepth,
                [&](const TypeExpr &typeExpr, const std::size_t nestedDepth) {
                  type(typeExpr, scope, nestedDepth);
                });
          if (!field.varName.empty()) scope.insert(field.varName);
        });
  }

  void contractDefault(const std::shared_ptr<ValueExpr> &defaultValue,
                       const Scope &scope, const std::size_t depth) {
    if (!defaultValue) throw ScanFailure("contract default is missing");
    pointer(defaultValue, depth,
            [&](const ValueExpr &expr, const std::size_t nestedDepth) {
              const auto *alias = std::get_if<std::string>(&expr.kind);
              if (alias)
                variable(*alias, scope, true);
              else
                value(expr, scope, nestedDepth);
            });
  }

  Scope contractPattern(const DestructurePattern &pattern, const Scope &scope,
                        const std::size_t depth) {
    Scope captures;
    sequence(pattern.fields, depth,
             [&](const DestructureField &field, const std::size_t childDepth) {
               if (field.defaultValue)
                 contractDefault(field.defaultValue, scope, childDepth);
               if (field.type) {
                 pointer(field.type, childDepth,
                         [&](const TypeExpr &typeExpr,
                             const std::size_t nestedDepth) {
                           auto nested = type(typeExpr, scope, nestedDepth);
                           captures.insert(nested.begin(), nested.end());
                         });
               }
               if (field.isContext) captures.insert(".");
               if (!field.varName.empty()) captures.insert(field.varName);
             });
    return captures;
  }

  Scope primaryType(const PrimaryType &primary, const Scope &scope,
                    const std::size_t depth) {
    tick(depth);
    Scope captures;
    std::visit(
        [&](const auto &kind) {
          using Kind = std::remove_cvref_t<decltype(kind)>;
          if constexpr (std::same_as<Kind, BasicType>) {
            if (kind.kind == BasicType::Kind::Contextual)
              refuse("contextual parameter types are not frame eligible");
          } else if constexpr (std::same_as<Kind, GenericType>) {
            sequence(kind.args, depth + 1,
                     [&](const std::shared_ptr<TypeExpr> &arg,
                         const std::size_t nestedDepth) {
                       pointer(arg, nestedDepth,
                               [&](const TypeExpr &nested,
                                   const std::size_t argDepth) {
                                 type(nested, scope, argDepth);
                               });
                     });
            if (kind.name != "Stream")
              refuse("contextual generic types are not frame eligible");
          } else if constexpr (std::same_as<Kind, ShapeType>) {
            captures = contractPattern(kind.pattern, scope, depth + 1);
          } else if constexpr (std::same_as<Kind, std::shared_ptr<TypeExpr>>) {
            pointer(kind, depth + 1,
                    [&](const TypeExpr &nested, const std::size_t nestedDepth) {
                      auto nestedCaptures = type(nested, scope, nestedDepth);
                      captures.insert(nestedCaptures.begin(),
                                      nestedCaptures.end());
                    });
          }
        },
        primary.kind);
    if (primary.refinement) {
      auto refined =
          contractPattern(primary.refinement->pattern, scope, depth + 1);
      if (!std::holds_alternative<GenericType>(primary.kind))
        captures.insert(refined.begin(), refined.end());
    }
    return captures;
  }

  Scope type(const TypeExpr &expr, const Scope &scope,
             const std::size_t depth) {
    tick(depth);
    Scope captures;
    sequence(expr.unionType.members, depth + 1,
             [&](const PrimaryType &primary, const std::size_t childDepth) {
               auto armCaptures = primaryType(primary, scope, childDepth);
               captures.insert(armCaptures.begin(), armCaptures.end());
             });
    return captures;
  }

  void pathLambda(const PathLambda &lambda, const Scope &outer,
                  const std::size_t depth) {
    if (lambda.params.size() > 1)
      refuse("path lambdas with multiple parameters are not frame eligible");

    Scope scope = outer;
    scope.insert(".");
    for (std::size_t index = 0; index < lambda.params.size(); ++index) {
      tick(depth);
      const auto &param            = lambda.params[index];
      const std::size_t childDepth = depth + 1;
      const Scope prematchScope    = scope;
      if (param.defaultValue) {
        const bool prior          = scanningParameterDefault_;
        scanningParameterDefault_ = true;
        pointer(param.defaultValue, childDepth,
                [&](const ValueExpr &expr, const std::size_t nestedDepth) {
                  value(expr, prematchScope, nestedDepth);
                });
        scanningParameterDefault_ = prior;
      }

      Scope typeCaptures;
      if (param.type)
        pointer(param.type, childDepth,
                [&](const TypeExpr &typeExpr, const std::size_t nestedDepth) {
                  typeCaptures = type(typeExpr, prematchScope, nestedDepth);
                });
      scope.insert(typeCaptures.begin(), typeCaptures.end());

      if (const auto *name = std::get_if<std::string>(&param.target)) {
        if (name->empty()) refuse("path lambda parameter name is empty");
        scope.insert(*name);
      } else if (std::holds_alternative<bool>(param.target)) {
        scope.insert(".");
      } else {
        const auto captures = contractPattern(
            std::get<DestructurePattern>(param.target), scope, childDepth);
        scope.insert(captures.begin(), captures.end());
      }
      if (param.isVariadic)
        refuse("variadic path lambdas are not frame eligible");
    }
    if (const auto *body =
            std::get_if<std::shared_ptr<ValueExpr>>(&lambda.body)) {
      pointer(*body, depth + 1,
              [&](const ValueExpr &expr, const std::size_t childDepth) {
                value(expr, scope, childDepth);
              });
    } else {
      pointer(std::get<std::shared_ptr<ScriptBody>>(lambda.body), depth + 1,
              [&](const ScriptBody &script, const std::size_t childDepth) {
                scriptBody(script, scope, childDepth);
              });
    }
  }

  void forClause(const ForClause &clause, Scope &scope,
                 const std::size_t depth) {
    pathExpression(clause.inPath, scope, depth + 1);
    if (clause.destructurePattern) {
      auto captures =
          contractPattern(*clause.destructurePattern, scope, depth + 1);
      scope.insert(captures.begin(), captures.end());
    } else if (!clause.varName.empty()) {
      scope.insert(clause.varName);
    }
  }

  void letClause(const LetClause &clause, Scope &scope,
                 const std::size_t depth) {
    std::visit(
        [&](const auto &target) {
          using Target = std::remove_cvref_t<decltype(target)>;
          if constexpr (std::same_as<Target, PathExpression>) {
            pathExpression(target, scope, depth + 1);
          } else {
            value(target, scope, depth + 1);
          }
        },
        clause.target);
    if (!clause.varName.empty()) scope.insert(clause.varName);
  }

  void returnClause(const ReturnClause &clause, const Scope &scope,
                    const std::size_t depth) {
    sequence(clause.items, depth,
             [&](const ReturnItem &item, const std::size_t childDepth) {
               std::visit(
                   [&](const auto &pathOrWeave) {
                     using Item = std::remove_cvref_t<decltype(pathOrWeave)>;
                     if constexpr (std::same_as<Item, PathExpression>) {
                       pathExpression(pathOrWeave, scope, childDepth);
                     } else {
                       refuse("field weaving is not frame eligible");
                       sequence(pathOrWeave.steps, childDepth,
                                [&](const PathStep &step,
                                    const std::size_t nestedDepth) {
                                  pathStep(step, scope, nestedDepth);
                                });
                     }
                   },
                   item.item);
             });
  }

  void effectClause(const EffectClause &clause, Scope &scope,
                    const std::size_t depth) {
    refuse("effect clauses are not reusable frame bodies");
    sequence(clause.items, depth,
             [&](const EffectItem &item, const std::size_t childDepth) {
               std::visit(
                   [&](const auto &part) {
                     using Part = std::remove_cvref_t<decltype(part)>;
                     if constexpr (std::same_as<Part, LetClause>) {
                       letClause(part, scope, childDepth);
                     } else if constexpr (std::same_as<Part, PathExpression>) {
                       pathExpression(part, scope, childDepth);
                     } else {
                       auto nestedScope = scope;
                       forClause(part.first, nestedScope, childDepth);
                       if (part.second)
                         pointer(part.second, childDepth,
                                 [&](const EffectClause &nested,
                                     const std::size_t nestedDepth) {
                                   effectClause(nested, nestedScope,
                                                nestedDepth);
                                 });
                       else
                         throw ScanFailure("nested effect clause is missing");
                     }
                   },
                   item.item);
             });
  }

  void actionClause(const ActionClause &action, Scope scope,
                    const std::size_t depth) {
    tick(depth);
    std::visit(
        [&](const auto &part) {
          using Part = std::remove_cvref_t<decltype(part)>;
          if constexpr (std::same_as<Part, ReturnClause>) {
            returnClause(part, scope, depth + 1);
          } else if constexpr (std::same_as<Part, EffectClause>) {
            effectClause(part, scope, depth + 1);
          } else {
            booleanExpr(part.condition, scope, depth + 1);
            if (!part.thenClause)
              throw ScanFailure("conditional then branch is missing");
            pointer(
                part.thenClause, depth + 1,
                [&](const ActionClause &branch, const std::size_t childDepth) {
                  actionClause(branch, scope, childDepth);
                });
            if (part.elseClause)
              pointer(part.elseClause, depth + 1,
                      [&](const ActionClause &branch,
                          const std::size_t childDepth) {
                        actionClause(branch, scope, childDepth);
                      });
          }
        },
        action.clause);
  }

  void executionBlock(const ExecutionBlock &block, Scope &scope,
                      const std::size_t depth) {
    Scope workingScope = scope;
    bool loopStarted   = false;
    sequence(block.bindings, depth + 1,
             [&](const auto &binding, const std::size_t childDepth) {
               std::visit(
                   [&](const auto &part) {
                     using Part = std::remove_cvref_t<decltype(part)>;
                     if constexpr (std::same_as<Part, ForClause>) {
                       if (!loopStarted) {
                         scope       = workingScope;
                         loopStarted = true;
                       }
                       forClause(part, workingScope, childDepth);
                     } else {
                       letClause(part, workingScope, childDepth);
                     }
                   },
                   binding);
             });
    if (block.where)
      booleanExpr(block.where->condition, workingScope, depth + 1);
    actionClause(block.action, workingScope, depth + 1);
    if (!loopStarted) scope = std::move(workingScope);
  }

  void statement(const Statement &statement, Scope &scope,
                 const std::size_t depth) {
    std::visit(
        [&](const auto &part) {
          using Part = std::remove_cvref_t<decltype(part)>;
          if constexpr (std::same_as<Part, ExecutionBlock>) {
            executionBlock(part, scope, depth + 1);
          } else if constexpr (std::same_as<Part, PathExpression>) {
            pathExpression(part, scope, depth + 1);
          } else if constexpr (std::same_as<Part, WeaveStatement>) {
            effectClause(part.effect, scope, depth + 1);
          } else if constexpr (std::same_as<Part, WatchStatement>) {
            refuse("watch statements are not reusable frame bodies");
            pathExpression(part.targetPath, scope, depth + 1);
            if (part.sinceVersion)
              versionExpr(*part.sinceVersion, scope, depth + 1);
            if (part.untilCondition)
              booleanExpr(*part.untilCondition, scope, depth + 1);
            auto nestedScope = scope;
            if (part.asPattern)
              destructure(*part.asPattern, nestedScope, depth + 1);
            if (part.asVar) nestedScope.insert(*part.asVar);
            if (part.body)
              pointer(
                  part.body, depth + 1,
                  [&](const ScriptBody &body, const std::size_t childDepth) {
                    scriptBody(body, nestedScope, childDepth);
                  });
          } else if constexpr (std::same_as<Part, PerspectiveBlock>) {
            refuse("perspective blocks are not reusable frame bodies");
            if (part.body)
              pointer(
                  part.body, depth + 1,
                  [&](const ScriptBody &body, const std::size_t childDepth) {
                    scriptBody(body, scope, childDepth);
                  });
          }
        },
        statement);
  }

  Scope scriptBody(const ScriptBody &body, Scope scope,
                   const std::size_t depth) {
    sequence(body.statements, depth + 1,
             [&](const Statement &item, const std::size_t childDepth) {
               statement(item, scope, childDepth);
             });
    return scope;
  }

  TraversalBudget &budget_;
  std::unordered_set<std::string> freeCaptureNames_;
  std::unordered_set<std::string> requiredDefaultCaptureBindings_;
  std::unordered_set<std::string> conditionalRequiredDefaultCaptureBindings_;
  std::vector<CallSite> callSites_;
  bool scanningParameterDefault_{false};
  bool scanningConditionalRestType_{false};
  std::optional<std::string> refusal_;
};

} // namespace

std::expected<FrameEligibility, FrameEligibilityError>
analyzeFrameEligibility(const QueryExpression &query,
                        const FrameEligibilityLimits limits) {
  FrameEligibility result;
  std::unordered_set<std::string> names;
  try {
    TraversalBudget budget(limits);
    std::unordered_map<std::string, std::vector<std::string>> callersByCallee;
    std::unordered_map<std::string, std::vector<CaptureDependency>>
        captureCallersByCallee;
    std::unordered_map<std::string, std::string> lazyDefaultRefusals;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        freeCaptureSets;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        requiredDefaultCaptureSets;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        conditionalRequiredDefaultCaptureSets;
    std::unordered_map<std::string, const DefDeclaration *> definitionsByName;
    for (const auto &definition : query.defs) {
      budget.tick(1);
      if (definition.name.empty())
        throw ScanFailure("frame eligibility found an unnamed definition");
      if (!names.insert(definition.name).second)
        throw ScanFailure("frame eligibility found duplicate definition names");
      definitionsByName.emplace(definition.name, &definition);
    }
    for (const auto &definition : query.defs) {
      DefinitionScanner scanner(budget);
      const auto reason = scanner.scan(definition);
      freeCaptureSets.emplace(definition.name, scanner.freeCaptureNames());
      requiredDefaultCaptureSets.emplace(
          definition.name, scanner.requiredDefaultCaptureBindings());
      conditionalRequiredDefaultCaptureSets.emplace(
          definition.name, scanner.conditionalRequiredDefaultCaptureBindings());
      auto callSites = std::move(scanner).takeCallSites();
      for (auto &callSite : callSites) {
        if (!names.contains(callSite.callee)) continue;
        budget.tick(1);
        callersByCallee[callSite.callee].push_back(definition.name);
        captureCallersByCallee[callSite.callee].push_back(
            {.caller                 = definition.name,
             .visibleNames           = std::move(callSite.visibleNames),
             .inParameterDefault     = callSite.inParameterDefault,
             .conditionalOnTypedRest = callSite.conditionalOnTypedRest,
             .mayInjectContext       = callSite.mayInjectContext,
             .argumentCount          = callSite.argumentCount});
      }
      if (reason)
        result.refusalReasons.emplace(definition.name, *reason);
      else
        result.reusableDefinitions.insert(definition.name);
    }

    std::deque<PendingCapture> pendingCaptures;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        queuedFreeCaptures;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        queuedRequiredDefaultCaptures;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        queuedConditionalDefaultCaptures;
    const auto enqueueCapture = [&](const std::string &definition,
                                    const std::string &name,
                                    const CaptureRequirement requirement) {
      auto &queued = requirement == CaptureRequirement::Free
                         ? queuedFreeCaptures[definition]
                     : requirement == CaptureRequirement::Required
                         ? queuedRequiredDefaultCaptures[definition]
                         : queuedConditionalDefaultCaptures[definition];
      if (queued.contains(name)) return;
      budget.tick(1);
      queued.insert(name);
      pendingCaptures.push_back({definition, name, requirement});
    };
    for (const auto &definition : query.defs) {
      const auto &freeCaptures = freeCaptureSets.at(definition.name);
      const auto &requiredCaptures =
          requiredDefaultCaptureSets.at(definition.name);
      for (const auto &name : freeCaptures)
        enqueueCapture(definition.name, name, CaptureRequirement::Free);
      for (const auto &name : requiredCaptures)
        enqueueCapture(definition.name, name, CaptureRequirement::Required);
      for (const auto &name :
           conditionalRequiredDefaultCaptureSets.at(definition.name))
        if (!requiredCaptures.contains(name))
          enqueueCapture(definition.name, name,
                         CaptureRequirement::ConditionalRequired);
    }

    while (!pendingCaptures.empty()) {
      budget.tick(1);
      auto current = std::move(pendingCaptures.front());
      pendingCaptures.pop_front();
      const auto callers = captureCallersByCallee.find(current.definition);
      if (callers == captureCallersByCallee.end()) continue;
      for (const auto &callSite : callers->second) {
        budget.tick(1);
        if (callSite.visibleNames.contains(current.name)) continue;

        bool requiredBindingActive =
            current.requirement == CaptureRequirement::Required;
        CaptureRequirement propagatedRequirement = CaptureRequirement::Free;
        if (current.requirement != CaptureRequirement::Free) {
          if (current.requirement == CaptureRequirement::ConditionalRequired) {
            const auto callee = definitionsByName.find(current.definition);
            if (callee == definitionsByName.end() ||
                callee->second->params.empty() ||
                !callee->second->params.back().isVariadic ||
                !callee->second->params.back().type)
              continue;
            const std::size_t fixedCount = callee->second->params.size() - 1;
            std::size_t actualCount      = callSite.argumentCount;
            if (callSite.mayInjectContext &&
                callee->second->params.size() > callSite.argumentCount)
              ++actualCount;
            if (actualCount <= fixedCount) continue;
            if (requiredDefaultCaptureSets.at(current.definition)
                    .contains(current.name))
              continue;
          }
          propagatedRequirement = callSite.conditionalOnTypedRest
                                      ? CaptureRequirement::ConditionalRequired
                                      : CaptureRequirement::Required;
          requiredBindingActive = true;
        }
        if (requiredBindingActive && callSite.inParameterDefault) {
          lazyDefaultRefusals.try_emplace(
              callSite.caller,
              "a required field-default capture is unavailable inside a "
              "lazy parameter default");
          continue;
        }

        const bool newFreeCapture =
            freeCaptureSets[callSite.caller].insert(current.name).second;
        bool newRequiredCapture = false;
        if (propagatedRequirement == CaptureRequirement::Required) {
          newRequiredCapture = requiredDefaultCaptureSets[callSite.caller]
                                   .insert(current.name)
                                   .second;
          conditionalRequiredDefaultCaptureSets[callSite.caller].erase(
              current.name);
        } else if (propagatedRequirement ==
                       CaptureRequirement::ConditionalRequired &&
                   !requiredDefaultCaptureSets[callSite.caller].contains(
                       current.name)) {
          newRequiredCapture =
              conditionalRequiredDefaultCaptureSets[callSite.caller]
                  .insert(current.name)
                  .second;
        }
        if (newFreeCapture)
          enqueueCapture(callSite.caller, current.name,
                         CaptureRequirement::Free);
        if (newRequiredCapture)
          enqueueCapture(callSite.caller, current.name, propagatedRequirement);
      }
    }

    for (const auto &definition : query.defs) {
      auto &freeCaptures  = result.freeCaptureNames[definition.name];
      const auto &freeSet = freeCaptureSets.at(definition.name);
      freeCaptures.assign(freeSet.begin(), freeSet.end());
      std::ranges::sort(freeCaptures);

      auto &requiredDefaults =
          result.requiredDefaultCaptureBindings[definition.name];
      const auto &requiredSet = requiredDefaultCaptureSets.at(definition.name);
      requiredDefaults.assign(requiredSet.begin(), requiredSet.end());
      std::ranges::sort(requiredDefaults);

      auto &conditionalDefaults =
          result.conditionalRequiredDefaultCaptureBindings[definition.name];
      const auto &conditionalSet =
          conditionalRequiredDefaultCaptureSets.at(definition.name);
      for (const auto &name : conditionalSet)
        if (!requiredSet.contains(name)) conditionalDefaults.push_back(name);
      std::ranges::sort(conditionalDefaults);
    }

    for (const auto &[caller, reason] : lazyDefaultRefusals) {
      if (result.reusableDefinitions.erase(caller))
        result.refusalReasons.emplace(caller, reason);
    }

    std::deque<std::string> ineligible;
    std::unordered_set<std::string> queued;
    for (const auto &[name, reason] : result.refusalReasons) {
      (void)reason;
      budget.tick(1);
      queued.insert(name);
      ineligible.push_back(name);
    }
    while (!ineligible.empty()) {
      budget.tick(1);
      const auto callee = std::move(ineligible.front());
      ineligible.pop_front();
      const auto callers = callersByCallee.find(callee);
      if (callers == callersByCallee.end()) continue;
      for (const auto &caller : callers->second) {
        budget.tick(1);
        if (!result.reusableDefinitions.erase(caller)) continue;
        result.refusalReasons.emplace(
            caller, "calls an inline-only definition: " + callee);
        if (queued.insert(caller).second) ineligible.push_back(caller);
      }
    }

    DefinitionScanner validator(budget);
    validator.validateQueryNodes(query);
  } catch (const std::exception &error) {
    return std::unexpected(FrameEligibilityError{error.what()});
  }
  return result;
}

} // namespace xanadu::vql

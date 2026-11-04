#include "common/xanadu/vql/frame_eligibility.hpp"

#include <algorithm>
#include <concepts>
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
    for (const auto &parameter : definition.params) {
      tick(1);
      if (parameter.isVariadic)
        refuse("variadic definitions remain on inline graph lowering");
      if (parameter.defaultValue) value(*parameter.defaultValue, scope, 2);
      if (parameter.type) type(*parameter.type, scope, 2);

      if (const auto *name = std::get_if<std::string>(&parameter.target)) {
        if (name->empty() || !scope.insert(*name).second)
          refuse("frame parameters must have unique variable names");
      } else if (const auto *context = std::get_if<bool>(&parameter.target)) {
        (void)context;
        refuse("context parameters remain on inline graph lowering");
      } else {
        const auto &pattern = std::get<DestructurePattern>(parameter.target);
        refuse("destructured parameters remain on inline graph lowering");
        destructure(pattern, scope, 2);
      }
    }

    if (definition.returnType) {
      auto returnScope = scope;
      type(*definition.returnType, returnScope, 1);
    }
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
                scriptBody(script, scope, depth);
              });
    }
    return refusal_;
  }

  [[nodiscard]] const std::unordered_set<std::string> &
  calledDefinitions() const noexcept {
    return calledDefinitions_;
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

  void variable(const std::string &name, const Scope &scope) {
    tick(0);
    if (name.empty() || !scope.contains(name))
      refuse("definition has a free lexical variable" +
             (name.empty() ? std::string{} : " $" + name));
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
          }
        },
        expr.kind);
  }

  void invocation(const FunctionInvocation &call, const Scope &scope,
                  const std::size_t depth) {
    tick(depth);
    calledDefinitions_.insert(call.name);
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
    sequence(path.steps, depth + 1,
             [&](const PathStep &step, const std::size_t childDepth) {
               pathStep(step, scope, childDepth);
             });
    if (path.shapeProjection) {
      shapeLiteral(*path.shapeProjection, scope, depth + 1);
      refuse("shape projections are not reusable frame bodies");
    }
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
            invocation(selector, scope, depth + 1);
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
                  refuse("path lambdas are not reusable frame bodies");
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
                    const std::size_t depth) {
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

  void primaryType(const PrimaryType &primary, Scope &scope,
                   const std::size_t depth) {
    tick(depth);
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
            refuse("shape contracts are not frame eligible");
            destructure(kind.pattern, scope, depth + 1);
          } else if constexpr (std::same_as<Kind, std::shared_ptr<TypeExpr>>) {
            pointer(kind, depth + 1,
                    [&](const TypeExpr &nested, const std::size_t nestedDepth) {
                      type(nested, scope, nestedDepth);
                    });
          }
        },
        primary.kind);
    if (primary.refinement) {
      refuse("shape contracts are not frame eligible");
      destructure(primary.refinement->pattern, scope, depth + 1);
    }
  }

  void type(const TypeExpr &expr, Scope &scope, const std::size_t depth) {
    tick(depth);
    sequence(expr.unionType.members, depth + 1,
             [&](const PrimaryType &primary, const std::size_t childDepth) {
               primaryType(primary, scope, childDepth);
             });
  }

  void pathLambda(const PathLambda &lambda, const Scope &outer,
                  const std::size_t depth) {
    Scope scope = outer;
    sequence(
        lambda.params, depth,
        [&](const LambdaParam &param, const std::size_t childDepth) {
          if (param.defaultValue)
            pointer(param.defaultValue, childDepth,
                    [&](const ValueExpr &expr, const std::size_t nestedDepth) {
                      value(expr, scope, nestedDepth);
                    });
          if (param.type)
            pointer(
                param.type, childDepth,
                [&](const TypeExpr &typeExpr, const std::size_t nestedDepth) {
                  type(typeExpr, scope, nestedDepth);
                });
          if (const auto *name = std::get_if<std::string>(&param.target))
            scope.insert(*name);
          else if (std::holds_alternative<DestructurePattern>(param.target))
            destructure(std::get<DestructurePattern>(param.target), scope,
                        childDepth);
          if (param.isVariadic) refuse("variadic path lambdas are unsupported");
        });
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
      refuse("destructured for-bindings are not frame eligible");
      destructure(*clause.destructurePattern, scope, depth + 1);
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
    sequence(block.bindings, depth + 1,
             [&](const auto &binding, const std::size_t childDepth) {
               std::visit(
                   [&](const auto &part) {
                     using Part = std::remove_cvref_t<decltype(part)>;
                     if constexpr (std::same_as<Part, ForClause>)
                       forClause(part, scope, childDepth);
                     else
                       letClause(part, scope, childDepth);
                   },
                   binding);
             });
    if (block.where) booleanExpr(block.where->condition, scope, depth + 1);
    actionClause(block.action, scope, depth + 1);
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

  void scriptBody(const ScriptBody &body, Scope scope,
                  const std::size_t depth) {
    sequence(body.statements, depth + 1,
             [&](const Statement &item, const std::size_t childDepth) {
               statement(item, scope, childDepth);
             });
  }

  TraversalBudget &budget_;
  std::unordered_set<std::string> calledDefinitions_;
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
    for (const auto &definition : query.defs) {
      budget.tick(1);
      if (definition.name.empty())
        throw ScanFailure("frame eligibility found an unnamed definition");
      if (!names.insert(definition.name).second)
        throw ScanFailure("frame eligibility found duplicate definition names");
    }
    for (const auto &definition : query.defs) {
      DefinitionScanner scanner(budget);
      const auto reason = scanner.scan(definition);
      for (const auto &callee : scanner.calledDefinitions()) {
        budget.tick(1);
        if (names.contains(callee))
          callersByCallee[callee].push_back(definition.name);
      }
      if (reason)
        result.refusalReasons.emplace(definition.name, *reason);
      else
        result.reusableDefinitions.insert(definition.name);
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

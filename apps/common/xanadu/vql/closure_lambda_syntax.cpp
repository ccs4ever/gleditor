#include "common/xanadu/vql/closure_lambda_syntax.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace xanadu::vql {
namespace {

struct ScanFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class SyntaxScanner {
public:
  explicit SyntaxScanner(const FrameEligibilityLimits limits)
      : limits_(limits) {
    if (limits_.maxNodes == 0 || limits_.maxDepth == 0)
      throw ScanFailure("closure lambda syntax budget is zero");
  }

  std::optional<ClosureLambdaSyntax> scan(const PathLambda &lambda) {
    tick(0);
    if (lambda.params.size() != 1) return std::nullopt;

    const auto &parameter = lambda.params.front();
    tick(1);
    const auto *name = std::get_if<std::string>(&parameter.target);
    if (!name || name->empty() || parameter.type || parameter.defaultValue ||
        parameter.isVariadic)
      return std::nullopt;

    ClosureLambdaSyntax result{.parameterName = *name};
    const auto *body = std::get_if<std::shared_ptr<ValueExpr>>(&lambda.body);
    if (!body) return std::nullopt;
    withPointer(*body, 1, [&](const ValueExpr &expr, const std::size_t depth) {
      if (!valueBody(expr, *name, depth)) supported_ = false;
    });
    if (!supported_) return std::nullopt;

    result.freeCaptureNames.assign(captures_.begin(), captures_.end());
    std::ranges::sort(result.freeCaptureNames);
    return result;
  }

private:
  void tick(const std::size_t depth) {
    if (depth > limits_.maxDepth)
      throw ScanFailure("closure lambda syntax depth budget exhausted");
    if (nodes_ >= limits_.maxNodes)
      throw ScanFailure("closure lambda syntax node budget exhausted");
    ++nodes_;
  }

  template <class T, class Action>
  void withPointer(const std::shared_ptr<T> &pointer, const std::size_t depth,
                   Action action) {
    if (!pointer)
      throw ScanFailure("closure lambda syntax found a null AST node");
    const void *identity = pointer.get();
    if (!activePointers_.insert(identity).second)
      throw ScanFailure("closure lambda syntax found a cyclic AST");
    struct EraseOnExit {
      std::unordered_set<const void *> &pointers;
      const void *identity;
      ~EraseOnExit() { pointers.erase(identity); }
    } erase{activePointers_, identity};
    tick(depth);
    action(*pointer, depth + 1);
  }

  void capture(const std::string &name) {
    if (name.empty()) throw ScanFailure("closure lambda has an empty alias");
    if (name != parameterName_) captures_.insert(name);
  }

  bool valueBody(const ValueExpr &expr, const std::string &parameter,
                 const std::size_t depth) {
    tick(depth);
    parameterName_ = parameter;
    if (const auto *name = std::get_if<std::string>(&expr.kind)) {
      if (name->empty()) throw ScanFailure("closure lambda has an empty alias");
      if (*name != parameter) captures_.insert(*name);
      return true;
    }
    if (const auto *path =
            std::get_if<std::shared_ptr<PathExpression>>(&expr.kind)) {
      return withPath(*path, parameter, depth + 1, false);
    }
    return false;
  }

  bool withPath(const std::shared_ptr<PathExpression> &path,
                const std::string &parameter, const std::size_t depth,
                const bool allowContextAnchor) {
    bool accepted = true;
    withPointer(
        path, depth,
        [&](const PathExpression &expression, const std::size_t childDepth) {
          accepted = pathExpression(expression, parameter, childDepth,
                                    allowContextAnchor);
        });
    return accepted;
  }

  bool pathExpression(const PathExpression &path, const std::string &parameter,
                      const std::size_t depth, const bool allowContextAnchor) {
    tick(depth);
    if (path.isFunctor || path.anchor.derefMaster || path.anchor.cellId != 0 ||
        path.anchor.createValue || path.anchor.generator ||
        path.anchor.storeAnchor || path.anchor.versionPin ||
        !path.anchor.microversionId.empty() || path.anchorProjection ||
        path.shapeProjection || path.cloneTail)
      return false;

    if (path.anchor.kind == AnchorKind::Variable) {
      capture(path.anchor.name);
    } else if (!(allowContextAnchor &&
                 path.anchor.kind == AnchorKind::Context &&
                 path.anchor.name.empty())) {
      return false;
    }

    for (const auto &predicate : path.anchorPredicates) {
      tick(depth + 1);
      if (!boolean(predicate, parameter, depth + 2)) return false;
    }
    for (const auto &step : path.steps) {
      tick(depth + 1);
      if (!pathStep(step, parameter, depth + 2)) return false;
    }
    return true;
  }

  bool pathStep(const PathStep &step, const std::string &parameter,
                const std::size_t depth) {
    tick(depth);
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    if (!dimension || dimension->dimName.empty() ||
        dimension->dimName.starts_with('$') ||
        !dimension->dimName.starts_with("d.") ||
        dimension->dimName.size() <= 2 || !dimension->creates.empty() ||
        step.derefMaster || step.versionPin || !step.shapePredicates.empty() ||
        step.projectionAfter || step.capturedFunctor ||
        step.yieldMode == YieldMode::Both ||
        step.yieldMode == YieldMode::Last || step.yieldMode == YieldMode::Keep)
      return false;
    for (const auto &predicate : step.predicates) {
      tick(depth + 1);
      if (!boolean(predicate, parameter, depth + 2)) return false;
    }
    return true;
  }

  bool boolean(const BooleanExpr &expr, const std::string &parameter,
               const std::size_t depth) {
    tick(depth);
    if (expr.terms.empty()) return false;
    for (const auto &term : expr.terms) {
      tick(depth + 1);
      if (term.factors.empty()) return false;
      for (const auto &factor : term.factors) {
        tick(depth + 2);
        const auto *comparison = std::get_if<ComparisonExpr>(&factor.test);
        if (comparison) {
          tick(depth + 3);
          if (!comparison->left || !comparison->right) return false;
          if (!comparisonValue(comparison->left, parameter, depth + 4) ||
              !comparisonValue(comparison->right, parameter, depth + 4))
            return false;
          continue;
        }
        const auto &test    = std::get<PredicateTest>(factor.test);
        const auto *context = std::get_if<bool>(&test.kind);
        if (!context || !*context) return false;
      }
    }
    return true;
  }

  bool comparisonValue(const std::shared_ptr<ValueExpr> &value,
                       const std::string &parameter, const std::size_t depth) {
    bool accepted = true;
    withPointer(
        value, depth, [&](const ValueExpr &expr, const std::size_t childDepth) {
          tick(childDepth);
          if (std::holds_alternative<ScalarLiteral>(expr.kind)) return;
          if (const auto *name = std::get_if<std::string>(&expr.kind)) {
            if (name->empty())
              throw ScanFailure("closure predicate has an empty alias");
            if (*name != parameter) captures_.insert(*name);
            return;
          }
          if (const auto *path =
                  std::get_if<std::shared_ptr<PathExpression>>(&expr.kind)) {
            accepted = withPath(*path, parameter, childDepth + 1, true);
            return;
          }
          accepted = false;
        });
    return accepted;
  }

  FrameEligibilityLimits limits_;
  std::size_t nodes_{0};
  bool supported_{true};
  std::string parameterName_;
  std::unordered_set<std::string> captures_;
  std::unordered_set<const void *> activePointers_;
};

} // namespace

std::expected<std::optional<ClosureLambdaSyntax>, FrameEligibilityError>
analyzeClosureLambdaSyntax(const PathLambda &lambda,
                           const FrameEligibilityLimits limits) {
  try {
    return SyntaxScanner{limits}.scan(lambda);
  } catch (const ScanFailure &error) {
    return std::unexpected(FrameEligibilityError{error.what()});
  }
}

} // namespace xanadu::vql

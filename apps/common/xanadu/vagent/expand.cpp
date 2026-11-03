#include "common/xanadu/vagent/expand.hpp"

#include "common/xanadu/vagent/code_path.hpp"

#include <optional>
#include <string>
#include <utility>

namespace xanadu::vagent {
namespace {

// The inner-offset reporting context: locs of the enclosing code"…"
// literals from the outermost in, each a position in its parent's text —
// the outermost in real source coordinates. A diagnostic raised inside
// the innermost literal's text wraps through innerDiagnostic once per
// level, innermost first, producing the src:m:l:c+o:c chain of §5.1. An
// empty chain is program-level reporting: the diagnostic keeps its own
// loc.
struct LiteralChain {
  std::vector<SourceLocation> literals;
};

class Expander {
public:
  Expander(Program &program, LiteralSeeds &seeds, ExpandLimits limits)
      : program_(program), seeds_(seeds), limits_(limits) {}

  std::expected<Expansion, std::vector<Diagnostic>> run() {
    for (auto &decl : program_.declarations) walkDecl(decl, {});
    if (!diagnostics_.empty()) return std::unexpected(std::move(diagnostics_));
    return Expansion{std::move(fragments_)};
  }

private:
  void report(Diagnostic diagnostic, const LiteralChain &chain) {
    for (auto it = chain.literals.rbegin(); it != chain.literals.rend(); ++it)
      diagnostic = innerDiagnostic(std::move(diagnostic), *it);
    diagnostics_.push_back(std::move(diagnostic));
  }

  void walkDecl(Decl &decl, const LiteralChain &chain) {
    for (auto &member : decl.members) walkDecl(member, chain);
    for (ExprPtr *slot : {&decl.initializer, &decl.body, &decl.guard,
                          &decl.trigger, &decl.deadline})
      if (*slot) walkExpr(**slot, chain, 0);
    for (auto &contract : decl.contracts)
      if (contract.condition) walkExpr(*contract.condition, chain, 0);
    for (auto &clause : decl.goalClauses)
      if (clause.condition) walkExpr(*clause.condition, chain, 0);
  }

  void walkExpr(Expr &expression, const LiteralChain &chain,
                std::size_t literalDepth) {
    switch (expression.kind) {
    case ExprKind::Code:
      literal(expression, chain, literalDepth);
      return;
    case ExprKind::Splice:
    case ExprKind::Static:
      // The phase-1 answer everywhere the grammar can place one —
      // program trees and parsed bodies alike — so a refused form can
      // never reach check() dressed as something else.
      if (++splices_ > limits_.check.maxSplices)
        report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                          "expansion exceeds CheckLimits::maxSplices",
                          expression.loc},
               chain);
      else
        report(Diagnostic{DiagnosticCode::Unsupported,
                          "'" +
                              std::string(expression.kind == ExprKind::Splice
                                              ? "splice"
                                              : "static") +
                              "' is refused by the phase-1 expand skeleton",
                          expression.loc},
               chain);
      return; // a refused node never composes, so its subtree earns no walk
    default:
      for (auto &child : expression.children)
        if (child) walkExpr(*child, chain, literalDepth);
      for (ExprPtr *slot :
           {&expression.scope, &expression.recipients, &expression.deadline,
            &expression.guard, &expression.recovery})
        if (*slot) walkExpr(**slot, chain, literalDepth);
      return;
    }
  }

  void literal(Expr &literalExpr, const LiteralChain &chain,
               std::size_t depth) {
    if (depth >= limits_.check.maxExpansionDepth) {
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "code\"…\" nesting exceeds "
                        "CheckLimits::maxExpansionDepth",
                        literalExpr.loc},
             chain);
      return;
    }
    // The literal's position inside its enclosing text — program
    // coordinates at the top level, parent-literal coordinates when
    // nested — is both the record key's loc half and the next chain arm.
    const FragmentKey key{literalExpr.loc, literalExpr.stamp
                                               ? literalExpr.stamp->ordinal
                                               : std::nullopt};
    // parseCodePath parses and narrows (return/suspension refusals are
    // its own, already inner-mapped); splice/static stay for this stage.
    auto parsed =
        parseCodePath(literalExpr.text, literalExpr.loc, limits_.parse);
    auto &slot = fragments_[key];
    if (parsed) {
      slot = std::move(*parsed);
    } else {
      // The diagnostic is already qualified at the literal — wrap it
      // through the enclosing literals only.
      report(std::move(parsed.error()), chain);
    }
    if (!slot.body) return;
    // Diagnostics inside the body are literal-relative: the chain gains
    // this literal's own loc as their first arm.
    LiteralChain body = chain;
    body.literals.push_back(literalExpr.loc);
    if (const auto seed = seeds_.byLiteral.find(key);
        seed != seeds_.byLiteral.end()) {
      auto resolved = resolveFragment(program_, *slot.body, seed->second,
                                      seeds_, limits_.resolve);
      if (!resolved)
        for (auto &diagnostic : resolved.error())
          report(std::move(diagnostic), body);
      // A missing seed means resolve never env-walked the literal's
      // position (a stampScopeOnly'd subtree, e.g. a callee): the body
      // still parses into a record and check()'s env walk — the scope
      // truth until phase 2's stamp honoring — checks it.
    }
    walkExpr(*slot.body, body, depth + 1);
  }

  Program &program_;
  LiteralSeeds &seeds_;
  ExpandLimits limits_;
  FragmentTable fragments_;
  std::vector<Diagnostic> diagnostics_;
  std::size_t splices_{0};
};

} // namespace

std::expected<Expansion, std::vector<Diagnostic>>
expand(Program &program, LiteralSeeds &seeds, ExpandLimits limits) {
  return Expander(program, seeds, limits).run();
}

} // namespace xanadu::vagent

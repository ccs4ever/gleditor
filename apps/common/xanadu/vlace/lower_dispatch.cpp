/**
 * @file lower_dispatch.cpp
 * @brief One dispatch per AST node kind (card L0). Every case names the card
 *        function it calls; nothing here lowers anything itself.
 */
#include <type_traits>
#include <variant>

#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

namespace {

/// Operators the dispatch routes away from lowerBinOp (L1).
constexpr std::string_view consOp   = ":";
constexpr std::string_view dollarOp = "$";
constexpr std::string_view ioBindOp = ">>=";
constexpr std::string_view ioThenOp = ">>";

Lowered<Operand> lowerBinary(LowerContext &ctx, const EBinOp &e,
                             SourceLocation loc) {
  if (e.op == consOp) return lowerCons(ctx, e, loc);     // L3
  if (e.op == dollarOp) return lowerDollar(ctx, e, loc); // L2
  if (e.op == ioBindOp || e.op == ioThenOp)
    return lowerIoBind(ctx, e, loc); // L6
  return lowerBinOp(ctx, e, loc);    // L1
}

Lowered<Operand> lowerCtorExpr(LowerContext &ctx, const ECtor &e,
                               SourceLocation loc) {
  if (e.name == "True") return lowerBoolCtor(ctx, true, loc);   // L1
  if (e.name == "False") return lowerBoolCtor(ctx, false, loc); // L1
  return lowerCtor(ctx, e, loc);                                // L3
}

} // namespace

Lowered<Operand> lowerExpr(LowerContext &ctx, const ExprNode &e) {
  const SourceLocation loc = e.loc;
  return std::visit(
      [&](const auto &node) -> Lowered<Operand> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ELiteral>)
          return lowerLiteral(ctx, node, loc); // L1
        else if constexpr (std::is_same_v<T, EVar>)
          return lowerVar(ctx, node, loc); // L1
        else if constexpr (std::is_same_v<T, ECtor>)
          return lowerCtorExpr(ctx, node, loc); // L1 / L3
        else if constexpr (std::is_same_v<T, EBinOp>)
          return lowerBinary(ctx, node, loc); // L1 / L2 / L3 / L6
        else if constexpr (std::is_same_v<T, EIf>)
          return lowerIf(ctx, node, loc); // L1
        else if constexpr (std::is_same_v<T, ELet>)
          return lowerLet(ctx, node, loc); // L2
        else if constexpr (std::is_same_v<T, ELambda>)
          return lowerLambda(ctx, node, loc); // L2
        else if constexpr (std::is_same_v<T, EApp>)
          return lowerApp(ctx, node, loc); // L2
        else if constexpr (std::is_same_v<T, ECase>)
          return lowerCase(ctx, node, loc); // L3
        else if constexpr (std::is_same_v<T, EList>)
          return lowerList(ctx, node, loc); // L3
        else if constexpr (std::is_same_v<T, ETuple>)
          return lowerTuple(ctx, node, loc); // L3
        else
          return lowerRecordUpdate(ctx, node, loc); // L4 (ERecordUpdate)
      },
      e.node);
}

Lowered<void> lowerPattern(LowerContext &ctx, const PatternNode &p,
                           PatternSite &site) {
  const SourceLocation loc = p.loc;
  return std::visit(
      [&](const auto &node) -> Lowered<void> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, PWildcard>)
          return {}; // matches anything, binds nothing
        else if constexpr (std::is_same_v<T, PVar>)
          return lowerPatVar(ctx, node, loc, site); // L3
        else if constexpr (std::is_same_v<T, PLiteral>)
          return lowerPatLiteral(ctx, node, loc, site); // L3
        else if constexpr (std::is_same_v<T, PConstructor>)
          return lowerPatConstructor(ctx, node, loc, site); // L3
        else if constexpr (std::is_same_v<T, PTuple>)
          return lowerPatTuple(ctx, node, loc, site); // L3
        else
          return lowerPatRecord(ctx, node, loc, site); // L4 (PRecord)
      },
      p.node);
}

Lowered<void> lowerDecl(LowerContext &ctx, const Decl &d, SourceLocation loc) {
  return std::visit(
      [&](const auto &node) -> Lowered<void> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ValueDecl>)
          return lowerValueDecl(ctx, node, loc); // L2
        else if constexpr (std::is_same_v<T, DataDecl>)
          return lowerDataDecl(ctx, node, loc); // L3
        else
          return {}; // type, import and fixity decls have no run-time effect
      },
      d);
}

Lowered<std::optional<Operand>> lowerProgram(LowerContext &ctx,
                                             const Program &prog) {
  for (const auto &decl : prog.decls) {
    if (auto r = lowerDecl(ctx, decl); !r) return std::unexpected(r.error());
  }
  if (!prog.result) return std::optional<Operand>{};
  auto result = lowerExpr(ctx, **prog.result);
  if (!result) return std::unexpected(result.error());
  return std::optional<Operand>{*result};
}

Lowered<Operand> resolveGlobalName(LowerContext &ctx, std::string_view name,
                                   SourceLocation loc) {
  if (auto r = lookupTopLevel(ctx, name, loc)) return *r;     // L2
  if (auto r = lowerFieldAccessor(ctx, name, loc)) return *r; // L4
  if (auto r = lowerIoBuiltin(ctx, name, loc)) return *r;     // L6
  if (auto r = lowerXanaduBuiltin(ctx, name, loc)) return *r; // L7
  if (auto r = lowerEffectBuiltin(ctx, name, loc)) return *r; // L8
  return fail(CompileError::UnboundIdentifier,
              std::string("unbound identifier '") + std::string(name) + "'",
              loc);
}

} // namespace xanadu::vlace::lower

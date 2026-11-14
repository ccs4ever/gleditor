/**
 * @file lower_support.hpp
 * @brief Small emission helpers shared by the verb-lowering files.
 *
 * These sit above graph_emit.hpp (they are built from it and LowerContext, and
 * add nothing to its frozen interface). They are here so that the same
 * run-time check, with the same message shape, is not copied into each card's
 * file.
 */
#ifndef COMMON_XANADU_VPL_LOWER_SUPPORT_HPP
#define COMMON_XANADU_VPL_LOWER_SUPPORT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

/// The value of a literal number as written: `3`, `2.5`, `¯3`, or `-3` (a
/// Minus over a literal). Nothing else is folded, so a computed operand is
/// std::nullopt.
[[nodiscard]] inline std::optional<double> literalNumber(const AstNode *node) {
  if (const auto *scalar = dynamic_cast<const ScalarExpr *>(node)) {
    if (scalar->isString()) return std::nullopt;
    return scalar->floatValue();
  }
  if (const auto *monadic = dynamic_cast<const MonadicExpr *>(node)) {
    if (monadic->customVerb() || monadic->verb() != TokenKind::Minus) {
      return std::nullopt;
    }
    if (const auto value = literalNumber(monadic->right().get())) {
      return -*value;
    }
  }
  return std::nullopt;
}

/// Emits an Assert on the cell `condition` with `message`: the VM stops with a
/// run-time failure carrying that text when the condition is false.
inline void assertCell(LowerContext &ctx, CellRef condition,
                       std::string_view label, std::string_view message) {
  const auto assertion = ctx.emitOp(zigzag::vortex::OpcodeKind::Assert, label);
  ctx.emitInput(assertion, condition);
  ctx.emitInputConstant(assertion, std::string{message});
}

/// Emits Assert(0 <= count) with `message`.
inline void assertNotNegative(LowerContext &ctx, CellRef count,
                              std::string_view label,
                              std::string_view message) {
  // Zero is the least count APL accepts; a smaller one is a DOMAIN ERROR.
  constexpr std::int64_t kLeastCount = 0;
  const auto valid                   = ctx.core().arena().makeCell();
  const auto test = ctx.emitOp(zigzag::vortex::OpcodeKind::Lte, label);
  ctx.emitInputConstant(test, kLeastCount);
  ctx.emitInput(test, count);
  ctx.emitOutput(test, valid);
  assertCell(ctx, valid, label, message);
}

/// The opcode and trace label of a scalar dyadic verb, as the scalar path of
/// compileDyadic emits it. One table, so scalar arithmetic, element-wise
/// arithmetic on ranks and fold cannot disagree about what a glyph means.
struct ScalarVerb {
  zigzag::vortex::OpcodeKind opcode;
  std::string_view label;
};

[[nodiscard]] inline std::optional<ScalarVerb>
scalarDyadicVerb(const TokenKind verb) {
  using zigzag::vortex::OpcodeKind;
  switch (verb) {
  case TokenKind::Plus:
    return ScalarVerb{OpcodeKind::Add, "#ADD"};
  case TokenKind::Minus:
    return ScalarVerb{OpcodeKind::Sub, "#SUB"};
  case TokenKind::Times:
    return ScalarVerb{OpcodeKind::Mul, "#MUL"};
  case TokenKind::Divide:
    return ScalarVerb{OpcodeKind::Div, "#DIV"};
  case TokenKind::Magnitude:
    return ScalarVerb{OpcodeKind::Mod, "#MOD"};
  case TokenKind::Ceiling:
    return ScalarVerb{OpcodeKind::Max, "#MAX"};
  case TokenKind::Floor:
    return ScalarVerb{OpcodeKind::Min, "#MIN"};
  case TokenKind::Equal:
    return ScalarVerb{OpcodeKind::Eq, "#EQ"};
  case TokenKind::NotEqual:
    return ScalarVerb{OpcodeKind::Neq, "#NEQ"};
  case TokenKind::LessThan:
    return ScalarVerb{OpcodeKind::Lt, "#LT"};
  case TokenKind::LessEqual:
    return ScalarVerb{OpcodeKind::Lte, "#LTE"};
  case TokenKind::GreaterThan:
    return ScalarVerb{OpcodeKind::Gt, "#GT"};
  case TokenKind::GreaterEqual:
    return ScalarVerb{OpcodeKind::Gte, "#GTE"};
  case TokenKind::And:
    return ScalarVerb{OpcodeKind::And, "#AND"};
  case TokenKind::Or:
    return ScalarVerb{OpcodeKind::Or, "#OR"};
  default:
    return std::nullopt;
  }
}

} // namespace xanadu::vpl::lower

namespace xanadu::vpl::lower {

struct Minting {
  zigzag::CellRef tail;
  zigzag::CellRef before;
  zigzag::DimRef dimension;
};

inline Minting beginResult(LowerContext &ctx, zigzag::DimRef dimension) {
  const auto anchor = ctx.core().arena().makeCell();
  const auto tail   = emit::extreme(ctx, anchor, dimension,
                                    zigzag::DimVector::POS, std::nullopt);
  return Minting{tail, emit::aliasRegister(ctx, tail), dimension};
}

inline zigzag::CellRef mintValue(LowerContext &ctx, const Minting &result,
                                 zigzag::CellRef number) {
  const auto fresh = emit::mintAfter(ctx, result.tail, result.dimension,
                                     zigzag::DimVector::POS);
  emit::copyValue(ctx, number, fresh);
  return fresh;
}

inline zigzag::CellRef endResult(LowerContext &ctx, const Minting &result) {
  using zigzag::vortex::OpcodeKind;
  const auto read = ctx.emitOp(OpcodeKind::Link, "#RESULT_FIRST");
  ctx.emitInput(read, result.before);
  ctx.emitInputConstant(read, static_cast<std::int64_t>(result.dimension));
  ctx.emitInputConstant(
      read, static_cast<std::int64_t>(zigzag::toSign(zigzag::DimVector::POS)));
  const auto first = ctx.core().arena().makeCell();
  ctx.emitOutput(read, first);
  return first;
}

} // namespace xanadu::vpl::lower

#endif // COMMON_XANADU_VPL_LOWER_SUPPORT_HPP

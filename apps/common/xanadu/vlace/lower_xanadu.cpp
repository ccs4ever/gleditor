/**
 * @file lower_xanadu.cpp
 * @brief Card L7: the pure `std:xanadu` arena verbs.
 *
 * Stubs written by card L0; refuses until L7 fills it in.
 */
#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

namespace {
constexpr std::string_view xanaduBuiltinNames[] = {
    "makeText", "makeDim", "dimensionNamed", "setLink",
    "cellText", "linked",  "childrenAll",    "words"};
} // namespace

std::optional<Lowered<Operand>>
lowerXanaduBuiltin(LowerContext &, std::string_view name, SourceLocation loc) {
  if (!isOneOf(name, xanaduBuiltinNames)) return std::nullopt;
  return notLowered("expressions", loc);
}

} // namespace xanadu::vlace::lower

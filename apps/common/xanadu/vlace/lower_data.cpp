/**
 * @file lower_data.cpp
 * @brief Card L3: constructors, tuples, lists, patterns, `case`, the built-in
 *        constructors, and the read-back and deep rendering of constructed
 *        values.
 *
 * Stubs written by card L0; each refuses until L3 fills it in. makeTerm and
 * termBuilder are called by L2 (partial constructors) and L6-L8 (IO and
 * `std:xanadu` terms) and must keep these signatures.
 */
#include <stdexcept>

#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

Lowered<Operand> lowerCtor(LowerContext &, const ECtor &, SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerTuple(LowerContext &, const ETuple &,
                            SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerList(LowerContext &, const EList &, SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerCons(LowerContext &, const EBinOp &, SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerCase(LowerContext &, const ECase &, SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<void> lowerPatVar(LowerContext &, const PVar &, SourceLocation loc,
                          PatternSite &) {
  return notLowered("patterns", loc);
}

Lowered<void> lowerPatLiteral(LowerContext &, const PLiteral &,
                              SourceLocation loc, PatternSite &) {
  return notLowered("patterns", loc);
}

Lowered<void> lowerPatConstructor(LowerContext &, const PConstructor &,
                                  SourceLocation loc, PatternSite &) {
  return notLowered("patterns", loc);
}

Lowered<void> lowerPatTuple(LowerContext &, const PTuple &, SourceLocation loc,
                            PatternSite &) {
  return notLowered("patterns", loc);
}

Lowered<void> lowerDataDecl(LowerContext &, const DataDecl &,
                            SourceLocation loc) {
  return notLowered("programs", loc);
}

void installBuiltinCtors(LowerState &) {}

Lowered<Operand> makeTerm(LowerContext &, std::string_view,
                          std::span<const Operand>, SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> termBuilder(LowerContext &, std::string_view, std::size_t,
                             SourceLocation loc) {
  return notLowered("expressions", loc);
}

std::optional<Lowered<ValuePtr>> readbackData(Compiler &, zigzag::CellRef) {
  return std::nullopt;
}

std::string renderData(Compiler &, const Value &, int) {
  throw std::logic_error(
      "vlace rendering is unavailable: VM evaluation is not implemented");
}

} // namespace xanadu::vlace::lower

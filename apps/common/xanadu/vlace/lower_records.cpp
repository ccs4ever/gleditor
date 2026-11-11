/**
 * @file lower_records.cpp
 * @brief Card L4: record construction and update, field accessors, record
 *        patterns.
 *
 * Stubs written by card L0; each refuses until L4 fills it in.
 */
#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

Lowered<Operand> lowerRecordUpdate(LowerContext &, const ERecordUpdate &,
                                   SourceLocation loc) {
  return notLowered("expressions", loc);
}

std::optional<Lowered<Operand>> lowerFieldAccessor(LowerContext &ctx,
                                                   std::string_view name,
                                                   SourceLocation loc) {
  // A name is a field accessor exactly when a record constructor owns it; the
  // table is filled by L3's lowerDataDecl, so before L3 no name is one.
  if (!ctx.state().ctors().fieldOwner(name)) return std::nullopt;
  return notLowered("expressions", loc);
}

Lowered<void> lowerPatRecord(LowerContext &, const PRecord &,
                             SourceLocation loc, PatternSite &) {
  return notLowered("patterns", loc);
}

} // namespace xanadu::vlace::lower

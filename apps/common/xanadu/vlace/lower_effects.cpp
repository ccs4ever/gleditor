/**
 * @file lower_effects.cpp
 * @brief Card L8: effect leaves (`promote`) and the lens verbs, plus the
 *        session-keyed reads `cellKey cellEq cellScalar`.
 *
 * Stubs written by card L0; each refuses until L8 fills it in.
 */
#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

namespace {
constexpr std::string_view effectBuiltinNames[] = {
    "promote",   "openLens",   "lensMount",  "lensUnmount", "lensNext",
    "lensStub",  "lensBrowse", "lensCommit", "lensRevert",  "lensEdit",
    "lensDirty", "cellKey",    "cellEq",     "cellScalar"};
/// The IO.* tags whose leaf effects L8 performs.
constexpr std::string_view effectLeafTags[] = {
    "IO.Promote",    "IO.OpenLens", "IO.LensMount",  "IO.LensUnmount",
    "IO.LensNext",   "IO.LensStub", "IO.LensBrowse", "IO.LensCommit",
    "IO.LensRevert", "IO.LensEdit", "IO.LensDirty"};
} // namespace

std::optional<Lowered<Operand>>
lowerEffectBuiltin(LowerContext &, std::string_view name, SourceLocation loc) {
  if (!isOneOf(name, effectBuiltinNames)) return std::nullopt;
  return notLowered("expressions", loc);
}

std::optional<Lowered<Operand>> performEffectLeaf(IoRunContext &,
                                                  std::string_view tag,
                                                  std::span<const Operand>,
                                                  SourceLocation loc) {
  if (!isOneOf(tag, effectLeafTags)) return std::nullopt;
  return notLowered("IO actions", loc);
}

} // namespace xanadu::vlace::lower

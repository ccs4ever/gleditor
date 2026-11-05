/**
 * @file epistemic_emit_shape.cpp
 * @brief The shape header of P19 T1b1: the record at the head of an agent's
 *        `d.epistemic` rank that names the layout of everything P19 keeps.
 */
#include <ranges>

#include "common/xanadu/vagent/runtime/compiler_gen.hpp"
#include "common/xanadu/vagent/runtime/epistemic_shape.hpp"

namespace xanadu::vagent::runtime::codegen {

/// Minted only for a unit with an arbitrated predicate, so an agent that never
/// arbitrates has an empty `d.epistemic` rank and nothing about it changes.
/// The cells belong to the program's own instantiation: a resume or an upgrade
/// compares the saved number with this one and never copies the saved header.
void Gen::mintShapeHeader() {
  if (!std::ranges::any_of(preds_,
                           [](const auto &p) { return p.second.arbitrated; }))
    return;
  const CellRef header = arena_.makeCell(kShapeHeaderName);
  const CellRef label  = arena_.makeCell(kShapeVersionField);
  zigzag::expectWritten(
      arena_.link(agentCell_, dims_.epistemic, DimVector::POS, header));
  zigzag::expectWritten(
      arena_.link(header, core_.dims().vars, DimVector::POS, label));
  zigzag::expectWritten(
      arena_.link(label, core_.dims().values, DimVector::POS,
                  arena_.makeScalarCell(kEpistemicShapeVersion)));
}

} // namespace xanadu::vagent::runtime::codegen

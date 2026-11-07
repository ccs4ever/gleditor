/**
 * @file epistemic_translate_ingress.cpp
 * @brief The agent-rank records signed ingress keeps (P19 I3-3a2): the
 *        audience identifier, read and planned for a transfer.
 *
 * The audience is carried by resume and upgrade alike (`i3-3-design.md` 8):
 * a device signs it into every message, so a new identifier after a resume
 * would refuse every device until each was reconfigured. The new program
 * mints its own shape header beside it; the copier appends the carried record
 * after that header.
 */
#include <algorithm>
#include <ranges>

#include "common/xanadu/vagent/runtime/epistemic_translate.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime {

namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::vortex::mesh::ActorError;

bool isAudienceToken(std::string_view text) {
  return text.size() == 64 && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

/// The record's token, when it is an audience record of the one shape the
/// routine writes: a single `token` field.
std::optional<std::string> tokenOf(const zigzag::ArenaManifold &arena,
                                   const ValueDims &vd, CellRef record) {
  const auto fields = fieldsOf(arena, vd, record);
  if (fields.size() != 1 ||
      arena.textOf(fields.front().first) != kAudienceTokenField ||
      !fields.front().second)
    return std::nullopt;
  auto token = arena.textOf(*fields.front().second);
  if (!isAudienceToken(token)) return std::nullopt;
  return token;
}

} // namespace

std::optional<std::string> audienceOf(const zigzag::ArenaManifold &arena,
                                      const CellRef agent,
                                      const DimRef epistemic,
                                      const ValueDims &vd) {
  for (const auto cell : zigzag::rankAfter(arena, agent, epistemic))
    if (arena.textOf(cell) == kAudienceRecordName)
      return tokenOf(arena, vd, cell);
  return std::nullopt;
}

std::expected<std::vector<CellRef>, TransferRefusal>
agentRankRecords(const zigzag::ArenaManifold &arena, const CellRef agent,
                 const OwnedGraphDims &dims, const TransferRoute route) {
  std::vector<CellRef> out;
  if (!dims.epistemic || !dims.vars || !dims.values) return out;
  const ValueDims vd{.vars   = *dims.vars,
                     .values = *dims.values,
                     .grab   = dims.grab.value_or(*dims.vars),
                     .step   = dims.step.value_or(*dims.vars)};
  const auto refuse = [&](TransferCode code, std::string subject) {
    return std::unexpected(
        TransferRefusal::make(code, route, ActorError::ExecutionFailed,
                              {.subject = std::move(subject)}));
  };
  for (const auto cell : zigzag::rankAfter(arena, agent, *dims.epistemic)) {
    const auto name = arena.textOf(cell);
    if (name == kShapeHeaderName) continue;
    if (name != kAudienceRecordName)
      return refuse(TransferCode::UnregisteredOwnedStructure, "d.epistemic");
    // One identifier per agent: two would leave the devices' choice to
    // whichever a reader met first.
    if (!out.empty() || !tokenOf(arena, vd, cell))
      return refuse(TransferCode::StateInconsistent,
                    std::string(kAudienceRecordName));
    out.push_back(cell);
  }
  return out;
}

} // namespace xanadu::vagent::runtime

#include "common/xanadu/vortex/owned_outcome.hpp" // IWYU pragma: associated

#include <algorithm>
#include <array>
#include <exception>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gleditor/logging.hpp>

namespace xanadu::vortex {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;

constexpr const char *logCategory = "vortex.io.script";

using Field = std::pair<std::string, std::string>;

/// The labelled fields on @p record's d.io.occurrence rank, in rank order.
auto fieldsOf(const ArenaManifold &arena, const IoCellSchema &schema,
              const CellRef record) -> std::vector<Field> {
  std::vector<Field> fields;
  const auto occurrence = schema.dim(IoDimension::Occurrence);
  const auto state      = schema.dim(IoDimension::State);
  auto at               = record;
  for (std::size_t visited = 0; visited <= arena.traversalBound(); ++visited) {
    const auto next = arena.linked(at, occurrence, DimVector::POS);
    if (!next) break;
    at = *next;
    if (const auto content = arena.linked(at, state, DimVector::POS))
      fields.emplace_back(arena.textOf(at), arena.textOf(*content));
  }
  return fields;
}

/// The Progress record cell of an outcome: outcome -progress-> occurrence
/// -progress-> record.
auto progressOf(const ArenaManifold &arena, const IoCellSchema &schema,
                const CellRef outcome) -> std::optional<CellRef> {
  const auto dim    = schema.dim(IoDimension::Progress);
  const auto holder = arena.linked(outcome, dim, DimVector::POS);
  if (!holder) return std::nullopt;
  return arena.linked(*holder, dim, DimVector::POS);
}

} // namespace

auto OwnedOutcome::copy(ArenaManifold &arena, const IoCellSchema &schema,
                        PayloadResolver &resolver, const OutcomeCell source,
                        std::optional<PayloadLease> payload)
    -> std::expected<OwnedOutcome, IoCellError> {
  const auto fail = [&](const IoCellError error) {
    if (payload) resolver.release(payload->lease);
    return std::unexpected{error};
  };
  const auto tag = ioOutcomeTag(arena, source);
  if (!tag) return fail(tag.error());
  auto observation    = outcomeCellReadObservation(arena, schema, source);
  const auto fields   = fieldsOf(arena, schema, source.cell);
  const auto progress = progressOf(arena, schema, source.cell)
                            .transform([&](const CellRef cell) {
                              return fieldsOf(arena, schema, cell);
                            })
                            .value_or(std::vector<Field>{});
  std::size_t chunkBytes = 0;
  if (payload) {
    const auto size = resolver.chunkBytes(*payload);
    if (!size) return fail(IoCellError::PayloadRefused);
    chunkBytes = *size;
  }

  const auto mark = arena.mark();
  try {
    const auto record = makeDetachedOutcomeCell(arena, schema, *tag);
    if (!record) {
      static_cast<void>(arena.release(mark));
      return fail(record.error());
    }
    bool written = true;
    for (const auto &[label, value] : fields) {
      written = written &&
                appendIoField(arena, record->cell,
                              schema.dim(IoDimension::Occurrence),
                              schema.dim(IoDimension::State), label, value);
    }
    written =
        written &&
        appendIoField(arena, record->cell, schema.dim(IoDimension::Occurrence),
                      schema.dim(IoDimension::State), ownedFieldLabel, "true");
    std::vector<std::pair<std::string_view, std::string_view>> views;
    views.reserve(progress.size());
    for (const auto &[label, value] : progress)
      views.emplace_back(label, value);
    written =
        written && addIoProgressFields(arena, schema, record->cell, views);
    if (!written || !arena.discard(mark)) {
      static_cast<void>(arena.release(mark));
      return fail(IoCellError::ArenaRefused);
    }
    return OwnedOutcome{.cell        = *record,
                        .payload     = std::move(payload),
                        .chunkBytes  = chunkBytes,
                        .observation = std::move(observation)};
  } catch (...) {
    static_cast<void>(arena.release(mark));
    if (payload) resolver.release(payload->lease);
    throw;
  }
}

auto OwnedOutcome::summary(ArenaManifold &arena, const IoCellSchema &schema,
                           const OutcomeCell source)
    -> std::expected<OwnedOutcome, IoCellError> {
  const auto tag = ioOutcomeTag(arena, source);
  if (!tag) return std::unexpected{tag.error()};
  // The fields a script branches on, by the names the cell store wrote.
  constexpr std::array kept{"errorCode", "stableDetail", "uncertain"};
  const auto fields = fieldsOf(arena, schema, source.cell);
  const auto mark   = arena.mark();
  try {
    const auto record = makeDetachedOutcomeCell(arena, schema, *tag);
    if (!record) {
      static_cast<void>(arena.release(mark));
      return std::unexpected{record.error()};
    }
    bool written = true;
    for (const auto &[label, value] : fields) {
      if (std::ranges::find(kept, label) == kept.end()) continue;
      written = written &&
                appendIoField(arena, record->cell,
                              schema.dim(IoDimension::Occurrence),
                              schema.dim(IoDimension::State), label, value);
    }
    written =
        written &&
        appendIoField(arena, record->cell, schema.dim(IoDimension::Occurrence),
                      schema.dim(IoDimension::State), ownedFieldLabel, "true");
    if (!written || !arena.discard(mark)) {
      static_cast<void>(arena.release(mark));
      return std::unexpected{IoCellError::ArenaRefused};
    }
    return OwnedOutcome{.cell = *record};
  } catch (...) {
    static_cast<void>(arena.release(mark));
    throw;
  }
}

void OwnedOutcome::releasePayload(PayloadResolver &resolver) noexcept {
  if (!payload) return;
  resolver.release(payload->lease);
  payload.reset();
  chunkBytes      = 0;
  payloadReleased = true;
}

void OwnedOutcome::freeCells(ArenaManifold &arena,
                             const IoCellSchema &schema) noexcept {
  freeOutcomeGraph(arena, schema, cell);
}

void OwnedOutcome::freeOutcomeGraph(ArenaManifold &arena,
                                    const IoCellSchema &schema,
                                    const OutcomeCell cell) noexcept {
  if (!arena.matches(cell.cell, cell.incarnation)) return;
  try {
    // The record's graph is exactly what hangs off it along the field,
    // content and progress dimensions; it has no clone, queue, waiter or lease
    // rank (copy() never makes one), so no cell here is anyone's master.
    constexpr std::array dims{IoDimension::Occurrence, IoDimension::State,
                              IoDimension::Progress};
    std::vector<CellRef> owned{cell.cell};
    std::unordered_set<CellRef> seen{cell.cell};
    for (std::size_t at = 0; at < owned.size(); ++at) {
      for (const auto which : dims) {
        const auto next =
            arena.linked(owned[at], schema.dim(which), DimVector::POS);
        if (next && seen.insert(*next).second) owned.push_back(*next);
      }
    }
    for (const auto doomed : owned | std::views::reverse) {
      const auto incarnation = arena.incarnationOf(doomed);
      if (!incarnation) continue;
      const auto deleted =
          arena.deleteCell(doomed, *incarnation,
                           zigzag::DeletionMode::LeaveBroken, std::nullopt);
      if (!deleted) {
        GLEDITOR_LOG_DEBUG(logCategory, "owned record cell kept: {}",
                           zigzag::toString(deleted.error()));
      }
    }
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(logCategory, "owned record release stopped: {}",
                      error.what());
  }
}

auto isOwnedOutcome(const ArenaManifold &arena, const IoCellSchema &schema,
                    const CellRef cell) -> bool {
  const auto incarnation = arena.incarnationOf(cell);
  if (!incarnation) return false;
  const auto tag = ioCellTag(arena, cell, *incarnation);
  if (!tag || *tag != IoCellTag::Outcome) return false;
  return std::ranges::any_of(
      fieldsOf(arena, schema, cell),
      [](const Field &field) { return field.first == ownedFieldLabel; });
}

} // namespace xanadu::vortex

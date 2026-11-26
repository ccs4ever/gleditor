#ifndef XANADU_VORTEX_OWNED_OUTCOME_HPP
#define XANADU_VORTEX_OWNED_OUTCOME_HPP

/**
 * @file owned_outcome.hpp
 * @brief A script-owned copy of a terminal outcome
 * (design/vortex-io-m7-vql-c.md §12.1), so a language can keep a named outcome
 * after the service has retired the request that produced it.
 *
 * The record is an `IoCellTag::Outcome` cell carrying the same tag, field rank
 * and Progress rank the service's outcome had, copied by value, plus one
 * `owned` field that marks it. It has no Request, Queue, Waiter, Clone or
 * Lease rank and so no road back to the service. A payload has no cell form,
 * so the record's bytes are a resolver lease held beside it, never a cell.
 * Language-neutral: nothing here knows VQL.
 */

#include <cstddef>
#include <expected>
#include <optional>

#include "common/xanadu/vortex/io_cells.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"

namespace xanadu::vortex {

/// The name of the field that marks a record as script-owned.
inline constexpr const char *ownedFieldLabel = "owned";

struct OwnedOutcome {
  OutcomeCell cell;
  /// The lease that keeps the retained bytes resolvable after the service
  /// releases its own; absent when the outcome retained none.
  std::optional<PayloadLease> payload;
  /// What the lease pins: the whole physical chunk (plan §8.1), which is what
  /// a retention budget charges.
  std::size_t chunkBytes{};
  /// The lease was given back (a finished script's record): the bytes it named
  /// are gone, which is not the same as a read that retained none.
  bool payloadReleased{};
  /// What the read observed (locator, generation, extent), copied by value
  /// from the service outcome's d.io.source cell: a capture of the record keys
  /// its overlap on the observation, which the record would otherwise have
  /// lost with the request. Absent for an outcome that carried none.
  std::optional<ReadObservationRecord> observation;

  /// The bytes the observation's own strings occupy, charged beside the
  /// payload chunk: the record keeps them for as long as it lives.
  [[nodiscard]] auto observationBytes() const noexcept -> std::size_t {
    return observation ? observation->locator.size() : 0;
  }

  /// Copy @p source's tag, scalar fields and progress into a new record. The
  /// copy takes ownership of @p payload: on any failure the lease is released
  /// and nothing is left behind. @p source is not touched.
  [[nodiscard]] static auto copy(zigzag::ArenaManifold &arena,
                                 const IoCellSchema &schema,
                                 PayloadResolver &resolver, OutcomeCell source,
                                 std::optional<PayloadLease> payload)
      -> std::expected<OwnedOutcome, IoCellError>;

  /// A smaller record of @p source: its tag and the three fields a script
  /// reads to decide what to do (the error code, its stable detail, the
  /// uncertain flag), and the `owned` mark. No payload, no observation and no
  /// Progress rank, so it mints the fewest cells a record can. It is the copy
  /// to fall back on when the full one is refused under arena pressure: a
  /// failed capture step must still say what it was (`TimedOut`, uncertain)
  /// and not be replaced by a generic refusal. Nothing here depends on the
  /// payload budget.
  [[nodiscard]] static auto summary(zigzag::ArenaManifold &arena,
                                    const IoCellSchema &schema,
                                    OutcomeCell source)
      -> std::expected<OwnedOutcome, IoCellError>;

  /// Give the payload lease back; idempotent.
  void releasePayload(PayloadResolver &resolver) noexcept;
  /// Delete the record's cells with the shared deletion algorithm (P7),
  /// leaves first, so a holder of the handle sees a stale cell and never a
  /// half-freed graph. Idempotent.
  void freeCells(zigzag::ArenaManifold &arena,
                 const IoCellSchema &schema) noexcept;
  /// The same deletion for any request-less outcome that holds no clone view:
  /// a service refusal (`Rejected`) whose submission has been released and
  /// which a script-owned copy now replaces.
  static void freeOutcomeGraph(zigzag::ArenaManifold &arena,
                               const IoCellSchema &schema,
                               OutcomeCell outcome) noexcept;
};

/// Whether @p cell is a script-owned record: an outcome carrying the `owned`
/// field.
[[nodiscard]] auto isOwnedOutcome(const zigzag::ArenaManifold &arena,
                                  const IoCellSchema &schema,
                                  zigzag::CellRef cell) -> bool;

} // namespace xanadu::vortex

#endif

#include "common/xanadu/vql/join_relation.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <expected>
#include <iterator>
#include <string>
#include <utility>
#include <variant>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::vortex::RelationAnswer;
using zigzag::vortex::RelationQuery;
using zigzag::vortex::RelationResult;

using Spans = std::vector<PrimediaSpan>;

/// A row's contribution, and whether it is the cell's own quoted content.
struct Piece {
  Spans spans;
  bool quoted{false};
};

[[nodiscard]] std::uint64_t bytesOf(const Spans &spans) noexcept {
  std::uint64_t total = 0;
  for (const auto &span : spans) total += span.length;
  return total;
}

/// The spans @p cell contributes: its own content when it quotes text, else
/// the canonical text of its value, interned. A structural handle has neither.
[[nodiscard]] std::expected<Piece, std::string>
spansOf(zigzag::vortex::VortexVM &vm, zigzag::ArenaManifold &arena,
        const CellRef cell) {
  const auto &core  = vm.core();
  const auto scalar = zigzag::readQueryScalar(
      arena, cell, core.dims().clone, core.dims().runtimeObjectClaim.value());
  if (!scalar) {
    return std::unexpected(
        scalar.error() == zigzag::QueryScalarReadError::UnsupportedHandle
            ? std::string{"join does not accept structural handles"}
            : std::string{"join input cell is unreadable"});
  }
  if (const auto *text = std::get_if<std::string>(&*scalar)) {
    // A foreign cell's bytes live in another manifold, so only a cell of this
    // arena can hand its spans on; the rest is copied as text.
    const auto master =
        arena.cloneMaster(cell, core.dims().clone).value_or(cell);
    const auto spans = arena.contentOf(master);
    if (!arena.foreignOf(master) && !spans.empty())
      return Piece{Spans(spans.begin(), spans.end()), true};
    if (text->empty()) return Piece{};
    return Piece{Spans{arena.intern(*text)}, false};
  }
  // The string alternative returned above, so what is left is a number or a
  // flag.
  const auto rendered = std::visit(
      [](const auto &value) -> std::string {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>,
                                   std::string>) {
          return value;
        } else {
          return xanadu::scalarValue(value).text;
        }
      },
      *scalar);
  return Piece{Spans{arena.intern(rendered)}, false};
}

} // namespace

zigzag::vortex::RelationProvider
joinStepProvider(zigzag::vortex::VortexVM &vm, std::shared_ptr<JoinWalk> walk) {
  return [&vm, walk = std::move(walk)](
             const RelationQuery &query) -> RelationResult {
    if (query.arguments.size() != 2)
      return std::unexpected("join_step expects a row and a separator");
    if (query.position == 0) *walk = {};
    const auto row = spansOf(vm, query.arena, query.arguments[0]);
    if (!row) return std::unexpected(row.error());
    Spans separator;
    if (query.position != 0) {
      auto taken = spansOf(vm, query.arena, query.arguments[1]);
      if (!taken) return std::unexpected(taken.error());
      separator = std::move(taken->spans);
    }
    const auto added = bytesOf(separator) + bytesOf(row->spans);
    if (query.limit < 0 ||
        walk->bytes + added > static_cast<std::uint64_t>(query.limit)) {
      return std::optional<RelationAnswer>{std::nullopt};
    }
    // An empty text can still hold a zero-length span; it names no byte, so
    // it would only make the run longer.
    for (const Spans *part :
         std::array<const Spans *, 2>{&separator, &row->spans}) {
      std::ranges::copy_if(
          *part, std::back_inserter(walk->runs),
          [](const PrimediaSpan &span) { return !span.empty(); });
    }
    walk->bytes += added;
    ++walk->rows;
    // A lone row is returned itself, so its quotation, and what links to it,
    // is not lost to a copy.
    walk->only = walk->rows == 1 && row->quoted
                     ? std::optional{query.arguments[0]}
                     : std::nullopt;
    return std::optional<RelationAnswer>{
        RelationAnswer{.nextPosition = query.position + 1,
                       .limit        = query.limit,
                       .answers = {query.arguments[0], query.arguments[1]}}};
  };
}

zigzag::vortex::RelationProvider
joinEndProvider(std::shared_ptr<JoinWalk> walk) {
  return [walk =
              std::move(walk)](const RelationQuery &query) -> RelationResult {
    if (query.arguments.size() != 1)
      return std::unexpected("join_end expects the separator");
    const auto finished = std::exchange(*walk, JoinWalk{});
    CellRef joined{};
    if (finished.only) {
      joined = *finished.only;
    } else if (finished.rows == 0) {
      joined = query.arena.makeCell(std::string_view{});
    } else {
      joined = query.arena.makeCell();
      if (const auto set = query.arena.setContent(joined, finished.runs); !set)
        return std::unexpected("join could not assemble its text");
    }
    return std::optional<RelationAnswer>{
        RelationAnswer{.nextPosition = query.position,
                       .limit        = query.limit,
                       .answers      = {joined}}};
  };
}

} // namespace xanadu::vql

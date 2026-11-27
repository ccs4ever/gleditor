#include "common/xanadu/vql/join_relation.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <expected>
#include <iterator>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gleditor/logging.hpp>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::vortex::RelationAnswer;
using zigzag::vortex::RelationQuery;
using zigzag::vortex::RelationResult;

using Spans = std::vector<PrimediaSpan>;

[[nodiscard]] std::uint64_t bytesOf(const Spans &spans) noexcept {
  std::uint64_t total = 0;
  for (const auto &span : spans) total += span.length;
  return total;
}

[[nodiscard]] bool isScratch(const PrimediaSpan &span) noexcept {
  return scratchScroll == span.scroll;
}

/// A row's contribution to the run, and whether the row quotes text, so that a
/// lone row can be handed back as it is.
struct Taken {
  JoinPiece piece;
  bool quotes{false};
};

/// What @p cell contributes: its own spans when it quotes text, with the store
/// they are addressed in, else the canonical text of its value, interned. A
/// structural handle has neither.
[[nodiscard]] std::expected<Taken, std::string>
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
  Taken taken{.piece = {.cell = cell, .master = cell}, .quotes = false};
  if (const auto *text = std::get_if<std::string>(&*scalar)) {
    const auto master =
        arena.cloneMaster(cell, core.dims().clone).value_or(cell);
    const auto spans   = arena.contentOf(master);
    taken.piece.master = master;
    taken.quotes       = !spans.empty();
    if (taken.quotes) {
      taken.piece.spans   = Spans(spans.begin(), spans.end());
      taken.piece.foreign = arena.foreignOf(master).has_value();
      if (const auto run = arena.quotedRun(master)) {
        taken.piece.store = run->store;
      }
      taken.piece.localOnly =
          std::ranges::all_of(taken.piece.spans, [](const PrimediaSpan &span) {
            return isScratch(span) || span.isLocal();
          });
      return taken;
    }
    if (!text->empty()) taken.piece.spans = Spans{arena.intern(*text)};
    return taken;
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
  taken.piece.spans = Spans{arena.intern(rendered)};
  return taken;
}

/// Whether every quoting piece can share one namespace, and whose.
///
/// The anchor is the store of the first piece with an external scroll (its ids
/// mean something only there), else of the first piece with a store; the
/// others agree when they are that store, or are all local and read the same
/// permascroll, which is `canCarry`'s own rule. A piece that names no store
/// and is all local (`makeCell(span)`, a capture) is the author's permascroll
/// as every other local span is, so it agrees with any anchor; a foreign
/// piece with no store, or a store-less one with an external scroll, has no
/// namespace to join and the rows are joined as text.
///
/// "Agrees with any anchor" is a precondition, not a check: a store-less local
/// span is trusted to be on the permascroll the anchor reads. Nothing at this
/// level can verify it (a span carries no permascroll identity), but every
/// store-less local cell the engine hands a query is a capture of the one
/// author permascroll, which is also what the anchor's store is over.
struct Namespace {
  bool shared{true};
  const Store *anchor{nullptr};
};

/// A piece whose spans mean the same bytes under any anchor.
[[nodiscard]] bool isFree(const JoinPiece &piece) noexcept {
  return piece.store == nullptr && !piece.foreign && piece.localOnly;
}

[[nodiscard]] Namespace namespaceOf(const std::vector<JoinPiece> &pieces) {
  std::vector<const JoinPiece *> quoting;
  for (const auto &piece : pieces) {
    if (std::ranges::any_of(piece.spans, [](const PrimediaSpan &span) {
          return !isScratch(span);
        })) {
      quoting.push_back(&piece);
    }
  }
  const auto stored = [](const JoinPiece *piece) {
    return piece->store != nullptr;
  };
  const auto free = [](const JoinPiece *piece) { return isFree(*piece); };
  if (std::ranges::any_of(quoting, [&](const JoinPiece *piece) {
        return !stored(piece) && !free(piece);
      })) {
    return {.shared = false};
  }
  const auto external = std::ranges::find_if(
      quoting, [&](const JoinPiece *p) { return stored(p) && !p->localOnly; });
  const auto first    = std::ranges::find_if(quoting, stored);
  const Store *anchor = external != quoting.end() ? (*external)->store
                        : first != quoting.end()  ? (*first)->store
                                                  : nullptr;
  const bool agree = std::ranges::all_of(quoting, [&](const JoinPiece *piece) {
    return free(piece) || piece->store == anchor ||
           (piece->localOnly &&
            &piece->store->userPermascroll() == &anchor->userPermascroll());
  });
  return {.shared = agree, .anchor = anchor};
}

} // namespace

zigzag::vortex::RelationProvider
joinStepProvider(zigzag::vortex::VortexVM &vm, std::shared_ptr<JoinWalk> walk) {
  return [&vm, walk = std::move(walk)](
             const RelationQuery &query) -> RelationResult {
    if (query.arguments.size() != 2)
      return std::unexpected("join_step expects a row and a separator");
    if (query.position == 0) *walk = {};
    auto row = spansOf(vm, query.arena, query.arguments[0]);
    if (!row) return std::unexpected(row.error());
    std::optional<Taken> separator;
    if (query.position != 0) {
      auto taken = spansOf(vm, query.arena, query.arguments[1]);
      if (!taken) return std::unexpected(taken.error());
      separator = std::move(*taken);
    }
    const auto added = (separator ? bytesOf(separator->piece.spans) : 0U) +
                       bytesOf(row->piece.spans);
    if (query.limit < 0 ||
        walk->bytes + added > static_cast<std::uint64_t>(query.limit)) {
      return std::optional<RelationAnswer>{std::nullopt};
    }
    // An empty text can still hold a zero-length span; it names no byte, so
    // it would only make the run longer.
    for (Taken *part : {separator ? &*separator : nullptr, &*row}) {
      if (part == nullptr) continue;
      std::erase_if(part->piece.spans,
                    [](const PrimediaSpan &span) { return span.empty(); });
      walk->pieces.push_back(std::move(part->piece));
    }
    walk->bytes += added;
    ++walk->rows;
    // A lone row is returned itself, so its quotation, and what links to it,
    // is not lost to a copy.
    walk->only = walk->rows == 1 && row->quotes
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
    auto &arena         = query.arena;
    CellRef joined{};
    if (finished.only) {
      joined = *finished.only;
    } else if (finished.rows == 0) {
      joined = arena.makeCell(std::string_view{});
    } else {
      // The rows' spans, in order, with the separators between them. Rows
      // of stores that cannot share one namespace are the text slice d
      // joined; the rest stay quoted.
      const auto shared = namespaceOf(finished.pieces);
      if (!shared.shared) {
        GLEDITOR_LOG_DEBUG(
            "vql.join", "join of {} rows: no shared namespace, joined as text",
            finished.rows);
      }
      Spans runs;
      for (const auto &piece : finished.pieces) {
        // Every piece that names bytes and is not proven free is typed,
        // whether or not it has a store: a store-less foreign or
        // external-scroll piece's raw spans would be read in the arena's
        // own namespace, which is the wrong bytes.
        const bool names =
            std::ranges::any_of(piece.spans, [](const PrimediaSpan &span) {
              return !isScratch(span);
            });
        if (!shared.shared && names && !isFree(piece)) {
          if (const auto text = arena.textOf(piece.master); !text.empty())
            runs.push_back(arena.intern(text));
        } else {
          runs.insert(runs.end(), piece.spans.begin(), piece.spans.end());
        }
      }
      std::optional<std::uint32_t> space;
      if (shared.shared && shared.anchor != nullptr) {
        for (std::uint32_t id = 1; id <= arena.spaceCount(); ++id) {
          if (const auto attached = arena.spaceAt(id);
              attached && attached->store == shared.anchor) {
            space = id;
            break;
          }
        }
      }
      if (space) {
        joined = arena.makeQuote(*space, runs);
      } else {
        joined = arena.makeCell();
        if (const auto set = arena.setContent(joined, runs); !set)
          return std::unexpected("join could not assemble its text");
      }
    }
    return std::optional<RelationAnswer>{
        RelationAnswer{.nextPosition = query.position,
                       .limit        = query.limit,
                       .answers      = {joined}}};
  };
}

} // namespace xanadu::vql

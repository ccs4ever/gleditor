/**
 * @file projection_session.cpp
 * @brief Implementation of the source-agnostic projection framework.
 */
#include "common/xanadu/zigzag/projection_session.hpp"

#include "common/xanadu/scalar.hpp"

#include <algorithm>
#include <bit>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zigzag {

namespace {

bool isStructuralDimName(std::string_view name) {
  return name == genericDimName(GenericDim::Containment) ||
         name == genericDimName(GenericDim::Sequence);
}

/// The node-content comparisons restateContent() needs. A scalar cell's
/// kind+bits are its content; a text cell's are its rendered bytes.
bool scalarEquals(const ArenaManifold &arena, CellRef cell, double v) {
  const auto d = arena.asDouble(cell);
  return d.has_value() &&
         xanadu::canonicalDoubleBits(*d) == xanadu::canonicalDoubleBits(v);
}

bool scalarEquals(const ArenaManifold &arena, CellRef cell, bool v) {
  const auto b = arena.asBool(cell);
  return b.has_value() && *b == v;
}

bool scalarEquals(const ArenaManifold &arena, CellRef cell, std::int64_t v) {
  const auto i = arena.asInt64(cell);
  return i.has_value() && *i == v;
}

} // namespace

ProjectionSession::ProjectionSession(std::unique_ptr<ProjectionLens> lens,
                                     zigzag::vortex::VortexCore &core)
    : lens_(std::move(lens)), core_(core) {
  project();
}

std::optional<CellRef>
ProjectionSession::cellFor(const ExternalKey &key) const noexcept {
  const auto it = keyToCell_.find(key);
  if (it == keyToCell_.end()) return std::nullopt;
  return it->second;
}

std::optional<ExternalKey>
ProjectionSession::keyFor(CellRef cell) const noexcept {
  const auto it = cellToKey_.find(cell);
  if (it == cellToKey_.end()) return std::nullopt;
  return it->second;
}

bool ProjectionSession::ownsCell(const CellRef cell) const noexcept {
  if (cellToKey_.contains(cell)) {
    return true;
  }
  // Attribute cells carry no key; writeAttribute hangs each exactly one
  // negward hop off its node on the attribute's rank. Read the cell's
  // own link run (dimensionsOf) rather than probing every managed dim,
  // and take exactly one step: an iterative climb loops on authored
  // 2-cycles, and an authored cell holding a node's negward slot steps
  // further negward -- away from the node -- so it never owns.
  const auto &arena = core_.arena();
  for (const DimLink &dl : arena.dimensionsOf(cell)) {
    const auto previous = arena.linked(cell, dl.dim, DimVector::NEG);
    if (managedDims_.contains(dl.dim) && previous &&
        cellToKey_.contains(*previous)) {
      return true;
    }
  }
  return false;
}

// -- small write helpers -----------------------------------------------------

DimRef ProjectionSession::dimFor(std::string_view name) {
  auto &arena       = core_.arena();
  const auto before = arena.cellCount();
  const DimRef dim  = core_.findOrMintDimension(name);
  mintCount_ += arena.cellCount() - before;
  managedDims_.insert(dim);
  return dim;
}

CellRef ProjectionSession::mintNodeCell(const SourceNode &node) {
  auto &arena        = core_.arena();
  const CellRef cell = std::visit(
      [&](const auto &c) -> CellRef {
        using T = std::remove_cvref_t<decltype(c)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return arena.makeCell();
        } else if constexpr (std::is_same_v<T, std::string>) {
          return arena.makeCell(std::string_view{c});
        } else if constexpr (std::is_same_v<T, double>) {
          return arena.makeScalarCell(c);
        } else if constexpr (std::is_same_v<T, bool>) {
          return arena.makeScalarCell(c);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return arena.makeScalarCell(c);
        }
      },
      node.content);
  ++mintCount_;
  return cell;
}

void ProjectionSession::mintNode(const SourceNode &node) {
  const CellRef cell = mintNodeCell(node);
  keyToCell_.emplace(node.key, cell);
  cellToKey_.emplace(cell, node.key);
  mintOrder_.push_back(node.key);
}

void ProjectionSession::writeAttribute(CellRef cell, std::string_view rank,
                                       std::string_view value) {
  auto &arena      = core_.arena();
  const DimRef dim = dimFor(rank);
  // Skip the mint when the rank already holds the same text -- an unchanged
  // attr rewritten every refresh would mint an orphan per cycle.
  if (const auto cur = arena.linked(cell, dim, Posward);
      cur.has_value() && arena.textOf(*cur) == value) {
    return;
  }
  CellRef attr = arena.makeCell(std::string_view{value});
  ++mintCount_;
  zigzag::expectWritten(
      arena.link(cell, dim, Posward, std::optional<CellRef>(attr)));
}

void ProjectionSession::restateContent(CellRef cell,
                                       const NodeContent &content) {
  auto &arena = core_.arena();
  // The restate is TOTAL: every branch restores both halves -- valueKind
  // takes the alternative's kind AND the span run takes the rendered
  // text or {}. Writing one without the other is how a type-changing
  // source value (or a reverted text edit on a scalar cell) left dead
  // scalar bits and stale edit text behind.
  const auto clearSpans = [&] {
    if (!arena.contentOf(cell).empty()) {
      const std::span<const xanadu::PrimediaSpan> empty{};
      zigzag::expectWritten(arena.setContent(cell, empty));
    }
  };
  std::visit(
      [&](const auto &c) {
        using T = std::remove_cvref_t<decltype(c)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          if (arena.valueKindOf(cell) != xanadu::ValueKind::None ||
              !arena.contentOf(cell).empty()) {
            zigzag::expectWritten(
                arena.setValueBits(cell, xanadu::ValueKind::None, 0));
            clearSpans();
          }
        } else if constexpr (std::is_same_v<T, std::string>) {
          if (arena.valueKindOf(cell) != xanadu::ValueKind::None ||
              arena.textOf(cell) != c) {
            zigzag::expectWritten(
                arena.setValueBits(cell, xanadu::ValueKind::None, 0));
            const xanadu::PrimediaSpan span = arena.intern(std::string_view{c});
            const std::span<const xanadu::PrimediaSpan> spans{&span, 1};
            zigzag::expectWritten(arena.setContent(cell, spans));
          }
        } else if constexpr (std::is_same_v<T, double>) {
          if (arena.valueKindOf(cell) != xanadu::ValueKind::Double ||
              !scalarEquals(arena, cell, c) || !arena.contentOf(cell).empty()) {
            zigzag::expectWritten(
                arena.setValueBits(cell, xanadu::ValueKind::Double,
                                   xanadu::canonicalDoubleBits(c)));
            clearSpans();
          }
        } else if constexpr (std::is_same_v<T, bool>) {
          if (arena.valueKindOf(cell) != xanadu::ValueKind::Bool ||
              !scalarEquals(arena, cell, c) || !arena.contentOf(cell).empty()) {
            zigzag::expectWritten(
                arena.setValueBits(cell, xanadu::ValueKind::Bool, c ? 1 : 0));
            clearSpans();
          }
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          if (arena.valueKindOf(cell) != xanadu::ValueKind::Int64 ||
              !scalarEquals(arena, cell, c) || !arena.contentOf(cell).empty()) {
            zigzag::expectWritten(
                arena.setValueBits(cell, xanadu::ValueKind::Int64,
                                   std::bit_cast<std::uint64_t>(c)));
            clearSpans();
          }
        }
      },
      content);
}

// -- project() / refresh() ---------------------------------------------------

void ProjectionSession::project() {
  // 1. Mint every declared dimension by its canonical name, so a query that
  //    names the same string resolves the same rank (§4.3.1).
  for (const auto &[name, generic] : lens_->dimensions()) {
    (void)generic;
    (void)dimFor(name);
  }
  // The first projection is a refresh() against an empty map: union-identity
  // mints every node, the edge pass links them, roots elect.
  (void)refresh();
}

std::size_t ProjectionSession::refresh() {
  auto &arena = core_.arena();
  // Internal mark: innermost, legal under outstanding scroll checkpoints --
  // a fault inside the loop releases cleanly rather than half-applying.
  const auto guard = arena.mark();

  std::unordered_set<ExternalKey> present;
  // keepPos[cell] = managed dims this enumeration re-emitted a POS-side link
  // on -- the tombstone's keep set.
  std::unordered_map<CellRef, std::unordered_set<DimRef>> keepPos;
  std::size_t minted = 0;

  // 1. Nodes: mint unseen keys (union identity), restate surviving content.
  lens_->forEachNode([&](const SourceNode &node) {
    present.insert(node.key);
    if (node.ref.has_value()) {
      // Project-by-reference (U-L5): the key names an existing cell --
      // bind it read-only; nothing below may write on the source's
      // structure (no restate, no attrs, no stale, no tombstone).
      if (keyToCell_.emplace(node.key, *node.ref).second) {
        cellToKey_.emplace(*node.ref, node.key);
        mintOrder_.push_back(node.key);
      }
      refKeys_.insert(node.key);
      return;
    }
    if (const auto it = keyToCell_.find(node.key); it != keyToCell_.end()) {
      // A dirty cell's arena content IS the pending edit -- restating
      // the source's emission over it would clobber what commit() is
      // about to send (and what the reader sees pre-commit).
      if (!dirty_.contains(node.key)) {
        restateContent(it->second, node.content);
      }
    } else {
      mintNode(node);
      ++minted;
    }
    const auto cell = cellFor(node.key);
    if (!cell) return;
    for (const auto &[rank, value] : node.attributes) {
      writeAttribute(*cell, rank, value);
      keepPos[*cell].insert(dimFor(rank));
    }
  });

  // 2. Edges: re-assert; a POS-direction emission keeps the from-cell's pos
  //    side; NEG-direction edges write the neg side (which tombstone leaves
  //    to reciprocity -- the far end's own pos link clears it).
  std::unordered_set<ExternalKey> containedTargets;
  lens_->forEachEdge([&](const SourceEdge &edge) {
    const auto from = cellFor(edge.from);
    const auto to   = cellFor(edge.to);
    if (!from || !to) return;
    const DimRef dim = dimFor(edge.dimension);
    zigzag::expectWritten(arena.link(*from, dim, edge.dir, to));
    if (edge.dir == DimVector::POS) {
      keepPos[*from].insert(dim);
      if (isStructuralDimName(edge.dimension)) {
        containedTargets.insert(edge.to);
      }
    }
  });

  // 3. Stale marking: keys absent from this enumeration get whatever the
  //    lens's staleAttributes() says (default {} -> untouched). Stale cells
  //    are NOT tombstoned -- nothing re-enumerates them.
  for (const auto &[key, cell] : keyToCell_) {
    if (present.contains(key) || refKeys_.contains(key)) continue;
    for (const auto &[rank, value] : lens_->staleAttributes(key)) {
      writeAttribute(cell, rank, value);
      // A just-written attr is asserted this epoch -- keep it out of
      // the absent-cell link dissolve below.
      keepPos[cell].insert(dimFor(rank));
    }
  }

  tombstone(present, keepPos);
  recomputeRoots(present, containedTargets);
  presentKeys_ = present; // revertKey's source-vanished signal

  arena.discard(guard);
  return minted;
}

bool ProjectionSession::refreshIfChanged() {
  if (!lens_->pollChanged()) {
    return false;
  }
  (void)refresh();
  return true;
}

std::expected<void, std::string>
ProjectionSession::editContent(const ExternalKey &key,
                               const std::string_view text) {
  if (refKeys_.contains(key)) {
    return std::unexpected("ref cells are read-only in the session (U-L5)");
  }
  const auto cell = cellFor(key);
  if (!cell) {
    return std::unexpected("unknown projected key: " + key);
  }
  if (!lens_->canApply(key)) {
    return std::unexpected("the lens reports " + key + " as not writable");
  }
  auto &arena = core_.arena();
  // A scalar-kind cell goes text-typed while dirty: the pending edit is
  // a span run, and commit()/revertKey()'s restate puts the source's
  // kind and content back over it.
  if (arena.valueKindOf(*cell) != xanadu::ValueKind::None) {
    zigzag::expectWritten(
        arena.setValueBits(*cell, xanadu::ValueKind::None, 0));
  }
  const xanadu::PrimediaSpan span = arena.intern(text);
  const std::span<const xanadu::PrimediaSpan> spans{&span, 1};
  zigzag::expectWritten(arena.setContent(*cell, spans));
  dirty_.insert(key);
  return {};
}

std::expected<void, std::string>
ProjectionSession::revertKey(const ExternalKey &key) {
  if (dirty_.erase(key) == 0) {
    return {}; // never dirtied -- a true no-op
  }
  // A key the source stopped emitting only stale-marks on refresh();
  // restateContent runs for present keys alone, so the edit text would
  // stay visible with nothing left to put back.
  if (!presentKeys_.contains(key)) {
    return std::unexpected("nothing to restate: " + key);
  }
  (void)refresh();
  return {};
}

std::expected<int, std::string> ProjectionSession::commit() {
  // Move the dirty set out, and guard it: every key the loop never
  // attempts goes back to dirty_ when this scope exits -- on a refusal
  // AND if applyContentChange throws, where the moved-out set would
  // otherwise destroy the rest. A key leaves `pending` only once its
  // apply succeeded (or its cell unwound); the guard re-inserts
  // whatever remains.
  auto pending = std::move(dirty_);
  dirty_.clear();
  struct ReinsertPending {
    ~ReinsertPending() { dst->insert(src->begin(), src->end()); }
    std::unordered_set<ExternalKey> *src;
    std::unordered_set<ExternalKey> *dst;
  } guard{&pending, &dirty_};

  int applied = 0;
  for (auto it = pending.begin(); it != pending.end();) {
    const auto key  = *it;
    const auto cell = cellFor(key);
    if (!cell) {
      // Unwound or removed between edit and commit: nothing to apply,
      // and re-dirtying a key with no cell would wedge the session.
      it = pending.erase(it);
      continue;
    }
    auto res = lens_->applyContentChange(key, core_.arena().textOf(*cell));
    if (!res) {
      // The refusing key stays dirty (it never left `pending`) and so
      // does the un-attempted tail -- a retry sees them all; the
      // caller decides what to drop.
      return std::unexpected(key + ": " + res.error());
    }
    it = pending.erase(it);
    ++applied;
    // A relocated key rebinds the same cell: union identity keeps the
    // cell, the source item's new name points at it. NOTE: the rebind
    // leaves a RouterLens's Mount::mountKey/roots/owns stale -- the
    // mount stays keyed on the old name until P8 remaps it; no lens
    // relocates today.
    if (*res != key) {
      const auto kit = keyToCell_.find(key);
      keyToCell_.erase(kit);
      keyToCell_.emplace(*res, *cell);
      cellToKey_.erase(*cell);
      cellToKey_.emplace(*cell, *res);
    }
  }
  (void)refresh();
  return applied;
}

void ProjectionSession::tombstone(
    const std::unordered_set<ExternalKey> &present,
    const std::unordered_map<CellRef, std::unordered_set<DimRef>> &keepPos) {
  auto &arena = core_.arena();
  for (const auto &key : present) {
    const auto cell = cellFor(key);
    if (!cell || refKeys_.contains(key)) continue;
    const auto keep = keepPos.find(*cell);
    // Snapshot the run: unlink() can relocate it mid-iteration.
    const auto run = arena.dimensionsOf(*cell);
    const std::vector<DimLink> snapshot(run.begin(), run.end());
    for (const DimLink &dl : snapshot) {
      if (!managedDims_.contains(dl.dim) ||
          !arena.linked(*cell, dl.dim, DimVector::POS)) {
        continue;
      }
      const bool kept = keep != keepPos.end() && keep->second.contains(dl.dim);
      if (!kept) {
        zigzag::expectWritten(arena.unlink(*cell, dl.dim, DimVector::POS));
      }
    }
  }
  // An absent cell's posward links are stale by definition: the source
  // emitted nothing from it this epoch, so anything it still links to
  // (a negward ghost on a live cell) comes down. The cell itself keeps
  // its staleAttributes -- those joined keepPos when they were written.
  for (const auto &[key, cell] : keyToCell_) {
    if (present.contains(key) || refKeys_.contains(key)) {
      continue;
    }
    const auto keep = keepPos.find(cell);
    const auto run  = arena.dimensionsOf(cell);
    const std::vector<DimLink> snapshot(run.begin(), run.end());
    for (const DimLink &dl : snapshot) {
      if (managedDims_.contains(dl.dim) &&
          arena.linked(cell, dl.dim, DimVector::POS) &&
          !(keep != keepPos.end() && keep->second.contains(dl.dim))) {
        zigzag::expectWritten(arena.unlink(cell, dl.dim, DimVector::POS));
      }
    }
  }
}

void ProjectionSession::recomputeRoots(
    const std::unordered_set<ExternalKey> &present,
    const std::unordered_set<ExternalKey> &containedTargets) {
  roots_.clear();
  if (lens_->forEachRoot([&](const ExternalKey &key) {
        if (const auto cell = cellFor(key); cell && present.contains(key)) {
          roots_.push_back(*cell);
        }
      })) {
    return; // lens-declared roots
  }
  for (const auto &key : mintOrder_) {
    if (present.contains(key) && !containedTargets.contains(key)) {
      if (const auto cell = cellFor(key)) roots_.push_back(*cell);
    }
  }
}

// -- checkpoint / rewind ------------------------------------------------------

ProjectionSession::Checkpoint ProjectionSession::checkpoint() {
  Checkpoint cp{.mark           = core_.arena().mark(),
                .keyHighWater   = mintOrder_.size(),
                .rootsHighWater = roots_.size(),
                .mintHighWater  = mintCount_,
                .boundaryKey    = {}};
  cps_.push_back(cp);
  return cp;
}

void ProjectionSession::pin(const ExternalKey &key) { pins_.insert(key); }

void ProjectionSession::unpin(const ExternalKey &key) { pins_.erase(key); }

std::expected<void, std::string>
ProjectionSession::rewind(const Checkpoint &cp) {
  auto &arena = core_.arena();

  // Find cp in the stack -- last match wins, so identical marks still mean
  // "the latest boundary with these counts".
  const auto it = std::ranges::find(cps_.rbegin(), cps_.rend(), cp);
  if (it == cps_.rend()) {
    return std::unexpected("unknown checkpoint");
  }
  const std::size_t idx =
      cps_.size() - 1 - static_cast<std::size_t>(it - cps_.rbegin());

  // Foreign marks above cp make the release() unsafe: outstanding marks must
  // equal our own checkpoints' count or something else holds the stack.
  if (arena.outstandingMarks() != cps_.size()) {
    return std::unexpected("foreign marks outstanding; cannot rewind");
  }

  // Pinned keys minted after cp block the toss -- refuse, naming the key.
  for (const auto &key : pins_) {
    const auto cell = keyToCell_.find(key);
    if (cell == keyToCell_.end()) continue;
    const auto pos = std::ranges::find(
        mintOrder_.begin() + static_cast<std::ptrdiff_t>(cp.keyHighWater),
        mintOrder_.end(), key);
    if (pos != mintOrder_.end()) {
      return std::unexpected("pinned key minted after checkpoint: " + key);
    }
  }

  // Foreign-mint check: the arena's cell delta above cp must equal the
  // session's logged mints -- anything else minted in the gap is a write a
  // checkpoint can't repair (solver answer cells, attach(Space) mints).
  const auto arenaDelta =
      arena.cellCount() - static_cast<std::size_t>(cp.mark.cellCount);
  const auto sessionDelta = mintCount_ - cp.mintHighWater;
  if (arenaDelta > sessionDelta) {
    return std::unexpected("foreign mint above checkpoint (dense index " +
                           std::to_string(cp.mark.cellCount + sessionDelta) +
                           " and beyond)");
  }

  // Pop cps above cp (discard, no undo), then release cp's mark.
  for (std::size_t i = cps_.size() - 1; i > idx; --i) {
    arena.discard(cps_[i].mark);
  }
  cps_.resize(idx + 1);
  arena.release(cp.mark);
  cps_.pop_back();

  // The lens-side discard is driven by the earliest node key minted above
  // the boundary -- capture before the log truncates.
  ExternalKey boundary = cp.keyHighWater < mintOrder_.size()
                             ? mintOrder_[cp.keyHighWater]
                             : ExternalKey{};

  // Map repair: every key minted after cp is forgotten from both maps.
  for (std::size_t i = cp.keyHighWater; i < mintOrder_.size(); ++i) {
    const auto cell = keyToCell_.find(mintOrder_[i]);
    if (cell != keyToCell_.end()) {
      cellToKey_.erase(cell->second);
      keyToCell_.erase(cell);
    }
  }
  mintOrder_.resize(cp.keyHighWater);
  roots_.resize(std::min(cp.rootsHighWater, roots_.size()));
  mintCount_ = cp.mintHighWater;

  // The lens-side half of the toss: a windowed lens drops buffered windows
  // so a replay -- never a buffer hit -- is the only way a window returns.
  if (auto *wl = dynamic_cast<WindowedLens *>(lens_.get())) {
    wl->discardMaterializedAfter(boundary);
  }
  return {};
}

} // namespace zigzag

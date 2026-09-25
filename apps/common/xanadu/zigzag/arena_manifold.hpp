/**
 * @file arena_manifold.hpp
 * @brief The ephemeral half of R8's two-type split: a manifold with no ops.
 *
 * Manifold is ops-backed -- every mutation appends a CompactOpNode and earns a
 * name in hypertime. That is correct and is what makes a document's history
 * navigable, and it is unaffordable for anything that mutates structure a
 * million times to answer one question. A query's intermediate states, a
 * cursor's position, a resolution engine's failed branches: none of them is a
 * change a person made to a document, so none of them may cost an operation.
 *
 * So the same cells and links, in the same CSR arenas, with the ops removed:
 *
 *   class Manifold;      // ops-backed. Every mutation appends a CompactOpNode.
 *   class ArenaManifold; // dense vectors only. No ops. Dies with its owner.
 *
 * Two consequences of there being no ops, both of which make this class
 * *simpler* than Manifold rather than a copy of it:
 *
 * - **A CellRef is an index, not an operation name.** Manifold has to resolve
 *   any index in a cell's micro-history chain (R7) to the cell it belongs to,
 *   which is what its byRef hash map is for. An arena cell has no history, so
 *   denseOf() here is arithmetic and there is no map.
 * - **Arena refs carry ephemeralBit.** Not as a convention: it makes every
 *   existing R8 refusal catch a leak with no new code. Store::setLink() throws
 *   on an ephemeral ref and Manifold::applyStructure() counts one as refused,
 *   so an arena ref that escapes into the persistent side is stopped by
 *   machinery written before this class existed. promote() is the only way
 *   across, and it works by mapping refs rather than by being trusted.
 *
 * See design/store-slice-convergence.md step 21 and R8, and
 * design/vlog-logic-extension.md §5.2-§5.5 for where mark()/release() and the
 * scratch scroll come from.
 *
 * **The overlay.** An arena may be given a base Manifold, and then it is a
 * copy-on-write view of a document rather than a blank space: reads of a cell
 * the arena does not hold fall through to the base, and the first *write* to
 * one shadows it -- the whole cell, its link run and its content run copied
 * into the arena, keeping the base's CellRef as its name. The base is never
 * touched, so resolving against a clause database living in a document costs
 * one copy per cell the evaluation actually writes and nothing for the rest.
 *
 * A shadow's identity is the base cell's ref, which is why CellSlot::birthOp is
 * the honest answer to "which cell is this slot" rather than refOf(dense): an
 * overlaid cell is the same cell, seen from a manifold that is allowed to
 * change its mind about it.
 */
#ifndef ZIGZAG_ARENA_MANIFOLD_HPP
#define ZIGZAG_ARENA_MANIFOLD_HPP

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gleditor/cpp26.hpp>
#include <gleditor/ranges.hpp>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {
class Store;
struct Scroll;
} // namespace xanadu

namespace zigzag {

/**
 * @brief One attached manifold, and what it takes to name a cell of it globally
 * (§5.7).
 */
struct Space {
  const Manifold *manifold{nullptr};
  std::shared_ptr<const Manifold> ownedManifold{nullptr};
  const xanadu::Store *store{nullptr};
  const xanadu::Scroll *sealedAs{
      nullptr}; ///< opRefOf() needs it; may be null locally
  const xanadu::SpanReader *reader{
      nullptr};              ///< owning scroll namespace; lifetime pinned
  CellRef storeCell{noCell}; ///< its cell on the d.stores rank
  std::string label;
};

using QuoteViewId =
    std::uint32_t; ///< one presentation occurrence, not cell identity

/**
 * @brief What a proxy stands for, or nothing for an ordinary arena cell (§5.7).
 */
struct ForeignRef {
  std::uint32_t space{0};
  CellRef index{noCell};

  bool operator==(const ForeignRef &) const  = default;
  auto operator<=>(const ForeignRef &) const = default;
};

enum class DimensionBindingMode : std::uint8_t {
  Explicit,
  NameMatch,
  SharedIdentity,
};

struct BoundDimensionMember {
  std::uint32_t space{0};
  DimRef dim{noCell};
  DimensionBindingMode mode{DimensionBindingMode::Explicit};

  bool operator==(const BoundDimensionMember &) const = default;
};

struct BoundDimensionSet {
  std::string name;
  DimRef arenaDim{noCell};
  std::vector<BoundDimensionMember> members;
};

struct ProxyEntry {
  std::uint32_t space{0};
  CellRef foreignIndex{noCell};
  CellRef proxy{noCell};
};

/**
 * @brief One overwritten CellSlot, and which cell it belonged to.
 *
 * The design note proposed `(cell, dim, negward, old)` -- a link's previous
 * value -- and that is not enough, for a reason only the CSR arena shows: a
 * cell that gains a dimension may have its whole link run *relocated* to the
 * arena's tail, which changes the slot header rather than any DimLink. Undoing
 * that by replaying link values would leave linkOffset pointing above the mark.
 *
 * Saving the slot covers it, and covers content runs, valueBits and lastOp in
 * the same mechanism, at 40 bytes instead of 16. Only cells older than the
 * innermost mark are ever saved, so the common case -- binding a variable this
 * clause activation minted -- still writes nothing.
 */
struct TrailEntry {
  std::uint32_t dense{0}; ///< which slot
  CellSlot saved{};       ///< what it held before the write
};
static_assert(sizeof(TrailEntry) == 40);

/**
 * @brief A choice point: the arena's high-water marks.
 *
 * Undo is truncation, so a choice point is the set of lengths to truncate back
 * to. Taking one allocates nothing and writes nothing, which is the property
 * that lets a resolution engine take a million of them (vlog §5.2); a
 * MicroversionId is a choice point too and cannot be taken a million times.
 *
 * liveLinks and liveContent are carried rather than recomputed so that
 * release() is O(trail) instead of O(cells): every slot above the mark is
 * discarded and every slot below it is restored, so the live counts return to
 * exactly what they were and can simply be assigned back.
 */
struct Mark {
  std::uint32_t cellCount{0};
  std::uint32_t linkSize{0};
  std::uint32_t contentSize{0};
  std::uint32_t scratchSize{0};
  std::uint32_t trailSize{0};
  std::uint32_t liveLinks{0};
  std::uint32_t liveContent{0};
  /// How many base cells had been shadowed. Release drops the shadows taken
  /// under the mark, so a cell the branch wrote to reverts to reading through
  /// the base -- which is the undo, for an overlaid cell.
  std::uint32_t shadowCount{0};
  std::uint32_t proxyCount{0};
  std::uint32_t quoteOccurrenceCount{0};
  std::uint32_t proxyShadowedEdgeCount{0};
};

/// What promote() refuses above, so that a runaway evaluation cannot write an
/// unbounded number of operations into a document.
/// Why an ArenaManifold write changed nothing.
enum class ArenaRefusal : std::uint8_t {
  UnknownCell,      ///< the subject is not a cell this arena can reach
  UnknownDimension, ///< the dimension is not a cell this arena can reach
  UnknownTarget,    ///< the far end is not a cell this arena can reach
  MarksOutstanding, ///< compact() inside a choice point would corrupt undo
};

/// An arena write: nothing on success, the reason it changed nothing if not.
using ArenaResult = std::expected<void, ArenaRefusal>;

[[nodiscard]] constexpr std::string_view
toString(const ArenaRefusal refusal) noexcept {
  switch (refusal) {
  case ArenaRefusal::UnknownCell:
    return "unknown cell";
  case ArenaRefusal::UnknownDimension:
    return "unknown dimension";
  case ArenaRefusal::UnknownTarget:
    return "unknown target";
  case ArenaRefusal::MarksOutstanding:
    return "marks outstanding";
  }
  return "unknown refusal";
}

/**
 * @brief Accept a write the caller has already made valid -- linking cells it
 *        just minted, say -- where a refusal would be the caller's bug.
 *
 * The alternative at such a site was to drop the result, which is exactly the
 * silence ArenaResult exists to end. A refusal here is logged on
 * `zigzag.arena` with the call site, so the bug shows up under SPDLOG_LEVEL
 * rather than as a structure quietly missing an edge.
 */
void expectWritten(
    const ArenaResult &result,
    std::source_location where = std::source_location::current()) noexcept;

struct PromotionBudget {
  std::uint32_t maxOps{65536};
};

/// What promote() produced: the state the store reached, and the real CellRef
/// for each arena cell it wrote, in the order they were minted.
struct Promoted {
  xanadu::MicroversionId version;
  std::vector<CellRef> cells;
};

/**
 * @class ArenaManifold
 * @brief Cells and links in dense vectors, with choice points and no history.
 */
class ArenaManifold {
public:
  ArenaManifold() = default;

  /// An arena over @p base: reads fall through, writes shadow. @p base must
  /// outlive this, and must not be mutated while it is being read through --
  /// a shadow copies a cell, not a promise about one.
  explicit ArenaManifold(const Manifold *base) : ArenaManifold(base, nullptr) {}

  /// Store-backed arena construction (§5.4 phase 4): overlays @p base and
  /// projects the authenticated publication provenance view as ephemeral
  /// cells if @p store holds verified provenance.
  ArenaManifold(const Manifold *base, const xanadu::Store *store);
  ArenaManifold(const Manifold *base, const xanadu::Store &store)
      : ArenaManifold(base, &store) {}

  [[nodiscard]] const Manifold *base() const noexcept { return base_; }
  [[nodiscard]] const xanadu::Store *store() const noexcept {
    return store_ != nullptr ? store_
                             : (base_ != nullptr ? base_->store() : nullptr);
  }
  [[nodiscard]] CellRef home() const noexcept;
  [[nodiscard]] CellRef homeCell() const noexcept { return home(); }

  [[nodiscard]] std::vector<DimRef> dimensions() const;

  [[nodiscard]] DimRef
  dimensionNamed(std::string_view name,
                 const xanadu::SpanReader *reader = nullptr) const;
  [[nodiscard]] std::optional<DimRef>
  dimensionNamed(std::string_view name, const xanadu::SpanReader &reader) const;
  DimRef ensureDimension(std::string_view name);

  [[nodiscard]] CellRef authorshipRoot() const noexcept {
    return authorshipRoot_;
  }
  [[nodiscard]] bool isProvenanceCell(CellRef ref) const noexcept {
    return provenanceCells_.contains(ref);
  }
  void projectProvenance(const xanadu::Store &store);

  // -- Federation (§5.7) ----------------------------------------------------

  /// Attach @p space: mints its store cell on d.stores and answers its 1-based
  /// id.
  std::uint32_t attach(Space space);

  [[nodiscard]] gleditor::cpp26::optional<const Space &>
  spaceAt(std::uint32_t id) const noexcept;
  [[nodiscard]] gleditor::cpp26::optional<Space &>
  spaceAt(std::uint32_t id) noexcept;

  [[nodiscard]] std::size_t spaceCount() const noexcept {
    return spaces_.size();
  }

  /// The proxy naming (@p space, @p foreignIndex), minting it on first ask.
  /// One canonical identity proxy per foreign cell; presentation is separate.
  CellRef proxyFor(std::uint32_t space, CellRef foreignIndex);

  /// Find existing proxy for (@p space, @p foreignIndex) without allocating;
  /// noCell if not yet minted.
  [[nodiscard]] CellRef findExistingProxy(std::uint32_t space,
                                          CellRef foreignIndex) const noexcept;

  /// Whether @p ref is a canonical federation proxy.
  [[nodiscard]] bool isProxy(CellRef ref) const noexcept;

  /// Light, view-local presentation cell; content and identity delegate to
  /// proxy. Its own ZigZag slots hold only edges induced by that quotation's
  /// selector.
  CellRef quoteOccurrence(QuoteViewId view, CellRef canonicalProxy);

  [[nodiscard]] bool isQuoteOccurrence(CellRef ref) const noexcept;
  [[nodiscard]] CellRef canonicalProxyOf(CellRef occurrence) const noexcept;

  /// What a proxy stands for, or nothing for an ordinary arena cell.
  [[nodiscard]] gleditor::cpp26::optional<ForeignRef>
  foreignOf(CellRef ref) const noexcept;

  /// The (store, local ref) a proxy names — what promote() and opRefOf() want.
  [[nodiscard]] gleditor::cpp26::optional<
      std::pair<const xanadu::Store *, CellRef>>
  resolveForeign(CellRef ref) const noexcept;

  /// Resolves the corresponding dimension in @p space for @p dim.
  [[nodiscard]] DimRef dimIn(std::uint32_t space, DimRef dim) const noexcept;

  /// Explicitly bind @p arenaDim to @p foreignDim in @p space.
  void
  bindDimension(DimRef arenaDim, std::uint32_t space, DimRef foreignDim,
                DimensionBindingMode mode = DimensionBindingMode::Explicit);

  /// Groups dimensions across attached spaces sharing the same published
  /// GlobalOpRef into BoundDimensionSet with SharedIdentity mode (§5.11 §5).
  void bindSharedIdentities();

  /// Groups dimensions across attached spaces sharing matching labels
  /// into BoundDimensionSet with NameMatch mode (§5.11 §5).
  void bindDimensionsByNameMatch();

  [[nodiscard]] gleditor::cpp26::optional<const BoundDimensionSet &>
  boundDimensionSet(DimRef dim) const noexcept;

  /// Pre-mints a frontier of canonical proxies along @p foreignDim starting at
  /// @p foreignHead.
  void materializeFrontier(std::uint32_t space, CellRef foreignHead,
                           DimRef foreignDim, DimVector dir = DimVector::POS,
                           std::size_t maxSteps = 100);

  void setAllocationLimitForTesting(std::uint32_t limit) noexcept {
    allocationLimit_ = limit;
  }
  [[nodiscard]] std::uint32_t allocationLimit() const noexcept {
    return allocationLimit_;
  }

  // Functional enumeration APIs leveraging C++26 function_ref
  void forEachSpace(
      gleditor::cpp26::function_ref<void(std::uint32_t, const Space &)> visitor)
      const;
  void forEachProxy(
      std::uint32_t space,
      gleditor::cpp26::function_ref<void(CellRef proxy, CellRef foreignIndex)>
          visitor) const;
  void forEachQuoteOccurrence(QuoteViewId view,
                              gleditor::cpp26::function_ref<void(
                                  CellRef occurrence, CellRef canonicalProxy)>
                                  visitor) const;

  /// Whether @p ref is a cell this arena minted or has shadowed, as opposed to
  /// one it is merely reading through. What promote() uses to decide whether a
  /// cell needs minting or already has a name.
  [[nodiscard]] bool holdsOwn(CellRef ref) const noexcept;

  // -- read path: the same questions Manifold answers ------------------------

  /// The dense id for @p ref, or npos. Arithmetic, not a lookup: an arena ref
  /// is ephemeralBit | dense and there are no chain indices to resolve.
  [[nodiscard]] std::uint32_t denseOf(CellRef ref) const noexcept;

  /// The ref for dense id @p dense. Public because promote() and a test both
  /// need to talk about "the n-th cell this arena minted".
  [[nodiscard]] static constexpr CellRef refOf(std::uint32_t dense) noexcept {
    return ephemeralBit | dense;
  }

  [[nodiscard]] SlotRef slot(CellRef ref) const noexcept;

  [[nodiscard]] bool contains(CellRef ref) const noexcept {
    return slot(ref).has_value();
  }

  [[nodiscard]] std::size_t cellCount() const noexcept { return slots_.size(); }

  /// How many cells a walk through this arena could visit: its own and every
  /// base cell it reads through. cellCount() alone would cut a walk over base
  /// cells short at the arena's own size.
  [[nodiscard]] std::size_t traversalBound() const noexcept {
    return slots_.size() + (nullptr == base_ ? 0 : base_->cellCount());
  }

  [[nodiscard]] std::span<const CellSlot> cells() const noexcept {
    return slots_;
  }

  /// The cell @p from's neighbour along @p dim, or noCell.
  [[nodiscard]] CellRef linked(CellRef from, DimRef dim,
                               DimVector dir = DimVector::POS) const noexcept;
  [[nodiscard]] CellRef linked(CellRef from,
                               DirectedDim target) const noexcept {
    return linked(from, target.dim, target.dir);
  }
  [[nodiscard]] CellRef linked(CellRef from, DimRef dim,
                               bool negward) const noexcept {
    return linked(from, dim, fromNegward(negward));
  }

  /// The dimensions @p ref links on -- d.meta-dims, read off the run.
  [[nodiscard]] std::span<const DimLink>
  dimensionsOf(CellRef ref) const noexcept;

  /// The spans @p ref's content is assembled from. Some may name scratchScroll.
  [[nodiscard]] std::span<const xanadu::PrimediaSpan>
  contentOf(CellRef ref) const noexcept;

  /**
   * @brief The group master of @p ref, following @p cloneDim negward.
   *
   * Vlog's `deref`, and the reason that document asks the core for so little:
   * this is Manifold::cloneMaster() over the arena's slots, cycle guard
   * included, because a rational term is a rank that loops and an engine over
   * one meets cycles in ordinary operation.
   */
  [[nodiscard]] std::optional<CellRef>
  cloneMaster(CellRef ref, DimRef cloneDim) const noexcept;

  [[nodiscard]] xanadu::ValueKind valueKindOf(CellRef ref) const noexcept;
  [[nodiscard]] std::optional<double> asDouble(CellRef ref) const noexcept;
  [[nodiscard]] std::optional<bool> asBool(CellRef ref) const noexcept;
  [[nodiscard]] std::optional<std::int64_t> asInt64(CellRef ref) const noexcept;
  [[nodiscard]] std::optional<CellRef> handleTarget(CellRef ref) const noexcept;

  /// @p ref's content as bytes. Scratch spans are read from this arena's own
  /// buffer; any other span needs @p reader, and is skipped when it is null.
  [[nodiscard]] std::string
  textOf(CellRef ref, const xanadu::SpanReader *reader = nullptr) const;
  [[nodiscard]] std::string textOf(CellRef ref,
                                   const xanadu::SpanReader &reader) const {
    return textOf(ref, &reader);
  }

  /// The bytes @p span names, when it is a scratch span this arena holds.
  [[nodiscard]] std::string_view
  scratchTextOf(const xanadu::PrimediaSpan &span) const noexcept;

  // -- write path: no operations, no names -----------------------------------

  /// Mint a bare cell -- no content, no value. A Vlog variable is one of these
  /// and nothing more; §3.2's "value_is_empty" is what makes it unbound.
  CellRef makeCell();

  /// Mint a cell quoting @p content, which already has an address somewhere.
  CellRef makeCell(const xanadu::PrimediaSpan &content);

  /// Mint a cell holding @p text, interned into the scratch buffer. The one
  /// operation that allocates bytes (vlog §5.5): atom_concat and friends.
  CellRef makeCell(std::string_view text);

  /**
   * @brief Mint a cell carrying a number, a flag or an integer -- bits only.
   *
   * Deliberately *not* both halves of R6. Store::makeScalarCell() spools the
   * shortest round-trip rendering as well, because a persisted scalar has to be
   * a link endpoint and a transclusion source; unification and arithmetic read
   * only the bits, so in an arena the rendering is deferred to promote(), and
   * an arithmetic-heavy evaluation allocates no content at all. The two halves
   * being independent is R6's, and this is the use it did not anticipate.
   */
  CellRef makeScalarCell(double value);
  CellRef makeScalarCell(bool value);
  CellRef makeScalarCell(std::int64_t value);

  /// Append @p text to the scratch buffer and answer its span.
  xanadu::PrimediaSpan intern(std::string_view text);

  /// Restate @p cell's content as @p spans. A run, not one span: U3's reason
  /// applies here too, and an arena cell can hold one where an op cannot yet.
  ArenaResult setContent(CellRef cell,
                         std::span<const xanadu::PrimediaSpan> spans);

  /// Restate @p cell's typed value, leaving its content alone.
  ArenaResult setValueBits(CellRef cell, xanadu::ValueKind kind,
                           std::uint64_t bits);

  /**
   * @brief Point @p from's @p dim-ward neighbour at @p to. noCell clears it.
   *
   * Maintains both ends and evicts whatever either end held, exactly as
   * Manifold::applyStructure()'s SetLink does -- which is what makes Vlog's
   * "unification is one link" literally one call rather than three and a
   * repair.
   *
   * @return an ArenaRefusal naming the ref this arena does not hold, having
   *         changed nothing.
   */
  ArenaResult link(CellRef from, DimRef dim, DimVector dir, CellRef to);
  ArenaResult link(CellRef from, DirectedDim target, CellRef to) {
    return link(from, target.dim, target.dir, to);
  }
  ArenaResult link(CellRef from, DimRef dim, bool negward, CellRef to) {
    return link(from, dim, fromNegward(negward), to);
  }

  // -- choice points ---------------------------------------------------------

  /// Take a choice point. Reads seven lengths; allocates nothing.
  [[nodiscard]] Mark mark() noexcept;

  /// Undo everything since @p m: replay its trail tail in reverse, then
  /// truncate. Cells minted under the mark are *gone*, not garbage.
  void release(const Mark &m) noexcept;

  /// Give up a choice point without undoing it -- success, or a cut. The
  /// bindings made under it stand; only the ability to retry is discarded.
  void discard(const Mark &m) noexcept;

  /// How many choice points are outstanding. Nonzero forbids compaction.
  [[nodiscard]] std::uint32_t outstandingMarks() const noexcept {
    return outstandingMarks_;
  }

  /// Entries the conditional trail holds. Stays at zero through a
  /// deterministic call that only binds variables it minted itself.
  [[nodiscard]] std::size_t trailSize() const noexcept { return trail_.size(); }

  /**
   * @brief Discard dead runs, leaving every cell's runs contiguous.
   *
   * **Refused while a mark is outstanding**, and this is load-bearing rather
   * than cautious: compaction moves every run, which is exactly what makes an
   * offset recorded in a Mark meaningless. An arena that compacted inside a
   * choice point would corrupt undo silently. Nothing is lost by waiting --
   * release() truncates the dead runs away by itself, so the runs a failed
   * branch left behind cost nothing to reclaim.
   *
   * @return ArenaRefusal::MarksOutstanding if it refused.
   */
  ArenaResult compact();

  /// Dead entries compact() would reclaim, for a test or a diagnostic.
  [[nodiscard]] std::size_t deadLinks() const noexcept {
    return links_.size() - liveLinks_;
  }

private:
  [[nodiscard]] DimLink *existingLink(std::uint32_t dense, DimRef dim) noexcept;
  [[nodiscard]] DimLink *linkFor(std::uint32_t dense, DimRef dim);
  void setOneSide(std::uint32_t dense, DimRef dim, DimVector dir, CellRef to);
  void setContentAt(std::uint32_t dense,
                    std::span<const xanadu::PrimediaSpan> spans);

  /// Save @p dense's slot if it is older than the innermost mark.
  ///
  /// The whole of §5.3's condition, and the WAM's conditional-trailing rule
  /// arrived at from the other direction: a cell younger than the mark is
  /// undone by the truncation that removes it, so trailing it would be
  /// recording an undo for something that will not exist. No attempt is made to
  /// notice a slot already saved under this mark -- a duplicate is harmless,
  /// because replaying in reverse restores the oldest value last, and the WAM
  /// does not deduplicate either.
  void trail(std::uint32_t dense);

  /// The dense slot for @p ref, copying the base's cell into the arena if that
  /// is where it still lives. noDense if @p ref is a cell of neither.
  ///
  /// Copy-on-write at cell granularity: the whole slot, its links and its
  /// content, so every later read of it is answered from one place and no read
  /// has to merge an override with what it overrides. An evaluation pays one
  /// copy per cell it writes to and nothing for the ones it only reads.
  [[nodiscard]] std::uint32_t shadow(CellRef ref);

  CellRef mintSlot(xanadu::ValueKind kind, std::uint64_t bits,
                   std::span<const xanadu::PrimediaSpan> content);

  std::vector<CellSlot> slots_;
  std::vector<DimLink> links_;
  std::vector<xanadu::PrimediaSpan> content_;
  /// Bytes constructed during evaluation, addressed by scratchScroll spans.
  std::string scratch_;
  std::vector<TrailEntry> trail_;

  /// The document being read through, or null for a standalone arena.
  const Manifold *base_{nullptr};
  const xanadu::Store *store_{nullptr};
  CellRef authorshipRoot_{noCell};
  std::unordered_set<CellRef> provenanceCells_;
  std::unordered_map<std::string, DimRef> arenaDims_;
  /// base CellRef -> the dense slot shadowing it.
  std::unordered_map<CellRef, std::uint32_t> overlay_;
  /// The same refs in the order they were shadowed, so release() can drop the
  /// ones a failed branch took. A map cannot be truncated; this can.
  std::vector<CellRef> shadowOrder_;

  std::size_t liveLinks_{0};
  std::size_t liveContent_{0};
  std::uint32_t outstandingMarks_{0};

  /// One outstanding mark's arena lengths: §5.3's comparands. `cells` is what
  /// decides whether a cell is old enough to trail; `links` and `content` are
  /// what decide whether it has already been copied out under this mark.
  ///
  /// A stack rather than one value, because the condition is about the
  /// *innermost* mark and nesting is how a resolution engine uses these. Marks
  /// are assumed well nested: release() and discard() pop.
  struct Floors {
    std::uint32_t cells{0};
    std::uint32_t links{0};
    std::uint32_t content{0};
  };
  std::vector<Floors> floors_;

  // Federation storage (§5.7)
  std::vector<Space> spaces_;
  std::vector<std::unordered_map<CellRef, CellRef>> spaceProxies_;
  std::vector<ProxyEntry> proxyOrder_;
  std::vector<CellRef> spaceStoreRefsTail_;

  std::unordered_map<CellRef, CellRef> occurrenceToCanonical_;
  std::unordered_map<CellRef, QuoteViewId> occurrenceToView_;
  std::map<std::pair<QuoteViewId, CellRef>, CellRef> viewCanonicalToOccurrence_;
  std::vector<CellRef> quoteOccurrenceOrder_;

  std::unordered_map<DimRef, BoundDimensionSet> boundDimensions_;

  struct ShadowEdgeKey {
    std::uint32_t dense{0};
    DimRef dim{noCell};
    DimVector dir{DimVector::POS};
    bool operator==(const ShadowEdgeKey &) const  = default;
    auto operator<=>(const ShadowEdgeKey &) const = default;
  };
  std::set<ShadowEdgeKey> proxyShadowedEdges_;
  std::vector<ShadowEdgeKey> proxyShadowedEdgeOrder_;
  CellRef storesRankTail_{noCell};
  std::uint32_t allocationLimit_{ephemeralBit};
};

/**
 * @brief Write an arena's answer into a document, as operations.
 *
 * The one road from the ephemeral side to the persistent one, and it works by
 * *mapping* refs rather than by being trusted with them: every arena ref is
 * ephemeralBit-tagged, so a promotion that forgot to translate one would be
 * refused by Store::setLink() rather than quietly persisting a dangling edge.
 *
 * Three things happen per reachable cell, in this order:
 *
 * 1. **A scratch span acquires a permanent address.** Constructed bytes are
 *    spooled into the author's permascroll and the span is rewritten to name
 *    it. This is where vlog §5.5's deferral is paid off.
 * 2. **A deferred scalar rendering is written.** An arena scalar carries bits
 *    only; Store::makeScalarCell() restores R6's other half.
 * 3. **The links are minted**, after every cell exists, because a link needs
 *    both of its ends.
 *
 * @param root the cell the answer hangs off. Only what is reachable from it
 *        along any dimension is promoted -- a failed branch's cells are not
 *        reachable from an answer, which is what makes "promote the answer,
 *        not the search" a graph walk rather than a bookkeeping problem.
 * @return nothing, having written nothing, when the reachable subgraph exceeds
 *         @p budget, or when @p root is not a cell of @p from.
 *
 * A cell whose content is a run of several spans is promoted as its first span
 * plus a splice per span after it, since one operation carries one span until
 * U3.4's CompactBinaryV4.
 */
[[nodiscard]] std::optional<Promoted>
promote(xanadu::Store &store, const xanadu::MicroversionId &parent,
        const ArenaManifold &from, CellRef root, PromotionBudget budget = {});

} // namespace zigzag

#endif // ZIGZAG_ARENA_MANIFOLD_HPP

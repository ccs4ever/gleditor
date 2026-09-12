/**
 * @file manifold.hpp
 * @brief The structure-map replay product: cells on ranks, folded from
 *        OpKind::Structure operations.
 *
 * A Version is what you get by replaying Insert/Delete/Rearrange/Transclude/
 * PageBreak over an ancestral path. A Manifold is what you get by folding
 * OpKind::Structure over the same path. Two replay products of one spool,
 * neither knowing about the other -- see design/store-slice-convergence.md §3.
 *
 * So a cell is not a new kind of object: it *is* an operation. Its identity is
 * that operation's name in hypertime, its content is the PrimediaSpan the
 * operation carries, and its positions are the Structure operations naming it.
 * Everything below is an index over facts the spool already holds, which is
 * why this class stores nothing that cannot be thrown away and rebuilt.
 */
#ifndef ZIGZAG_MANIFOLD_HPP
#define ZIGZAG_MANIFOLD_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu {
class Store;
} // namespace xanadu

namespace zigzag {

// CellRef, DimRef, and noCell are defined in dim_vector.hpp

/**
 * @brief The top bit: this cell is derived and no operation backs it.
 *
 * d.meta-dims is the example the model is built around -- a cell's run of
 * dimensions, read sideways as a rank of clone cells -- and it is generated
 * rather than stored, so its cells cannot have an operation index for a name.
 *
 * This does double duty as the byte-level enforcement of R8: applyStructure()
 * refuses a SetLink naming an ephemeral cell, so "a link into a derived cell
 * cannot be persisted" is checked by the encoding rather than trusted to the
 * caller.
 */
inline constexpr CellRef ephemeralBit = 0x8000'0000U;

[[nodiscard]] constexpr bool isEphemeral(const CellRef ref) noexcept {
  return (ref & ephemeralBit) != 0;
}

/**
 * @brief A cell's two neighbours along one dimension.
 *
 * No dimension is privileged: `dim` is an ordinary CellRef, so a dimension VQL
 * minted this frame costs exactly what d.1 costs. An earlier draft kept a
 * fixed inline array of eight well-known dimensions, which put the privilege
 * back at the storage layer one level down where it is harder to see. It
 * measured 2.7x faster per hop and was removed anyway -- see R12 for the
 * arithmetic, and for the traversal size at which that ruling should be
 * revisited.
 */
struct DimLink {
  DimRef dim{noCell};  ///< the dimension cell
  CellRef pos{noCell}; ///< posward neighbour, noCell if none
  CellRef neg{noCell}; ///< negward neighbour, noCell if none

  [[nodiscard]] constexpr CellRef neighbor(const DimVector dir) const noexcept {
    return dir == DimVector::POS ? pos : neg;
  }
  [[nodiscard]] constexpr CellRef &neighbor(const DimVector dir) noexcept {
    return dir == DimVector::POS ? pos : neg;
  }

  bool operator==(const DimLink &) const = default;
};
static_assert(sizeof(DimLink) == 12);

/**
 * @brief What is known about one cell, without its links.
 *
 * The links are a contiguous run in the manifold's one arena, addressed by
 * @ref linkOffset and @ref linkCount -- which is also this cell's d.meta-dims,
 * exactly and without a cap.
 */
struct CellSlot {
  /**
   * @brief This cell's content, as a **run** of spans in the manifold's content
   *        arena -- not one inline span.
   *
   * One span could not survive an edit. `setCellText()` had to re-spool the
   * whole content and repoint the cell, so changing one byte of a thousand
   * moved all thousand to a new address and severed every transclusion that
   * shared the old one -- the measurement is in U3. A run keeps the addresses
   * of the text that did not change, which is what makes an edit an edit rather
   * than a replacement.
   *
   * The run is a piece table with `Version`'s semantics and none of its
   * storage: `Version` holds a `std::vector` per document, which at one per
   * cell is the per-cell heap block §6.2 measured a 122x rank-hop penalty for.
   * The spans live in one arena shared by every cell, addressed here the same
   * way the links are.
   */
  std::uint32_t spanOffset{0}; ///< 4: first span of this cell's content run
  std::uint16_t spanCount{0};  ///< 2: length of that run

  std::uint32_t birthOp{0};    ///< 4: the MakeCell index; == this CellRef
  std::uint32_t lastOp{0};     ///< 4: head of the micro-history chain (R7)
  std::uint32_t linkOffset{0}; ///< 4: first DimLink of this cell's run
  std::uint16_t linkCount{0};  ///< 2: length of the run
  std::uint8_t valueKind{0};   ///< 1: xanadu::ValueKind of @ref valueBits
  std::uint8_t flags{0};       ///< 1: unclaimed
  std::uint64_t valueBits{0};  ///< 8: canonical scalar bits (R6)

  bool operator==(const CellSlot &) const = default;
};
// Thirty-two, down from forty-eight: an inline 24-byte PrimediaSpan became a
// six-byte reference into an arena. A cell with one span is therefore 32 + 24
// against the 48 it was, and every span after the first costs 24 rather than
// being impossible.
static_assert(sizeof(CellSlot) == 32);

/**
 * @class Manifold
 * @brief A zzstructure over one store's cells: an explicitly materialised view.
 *
 * Not a pretend-pure replay product. Folding Structure over an ancestral path
 * is O(history), and store.hpp guarantees that nothing caches versions, so
 * this one says out loud what it is: fold once from the ancestral path
 * (Store::rebuildManifold), then drive it one operation at a time with
 * advance() or applyStructure(). verifyAgainstFullRebuild() is the honesty
 * mechanism that keeps the incremental state from drifting away from a cold
 * fold the way UnifiedTransclusionEngine::cells_ can. See R9.
 *
 * Nothing in the tree consumes this yet.
 */
class Manifold {
public:
  // -- read path: no allocation, no locks, no Store access -------------------

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

  /**
   * @brief The slot for @p ref, or nullptr.
   *
   * Accepts any operation index in a cell's micro-history chain, not only its
   * birth op: a SetLink names its subject by chaining to the previous
   * operation on the same cell (R7), so resolving a chain index to the cell it
   * belongs to is something this class has to be able to do anyway.
   */
  [[nodiscard]] const CellSlot *slot(CellRef ref) const noexcept;

  /// Whether @p ref names a cell this manifold holds.
  [[nodiscard]] bool contains(CellRef ref) const noexcept {
    return nullptr != slot(ref);
  }

  /// How many cells there are.
  [[nodiscard]] std::size_t cellCount() const noexcept { return slots.size(); }

  /// Every cell, in the order the fold minted them.
  [[nodiscard]] std::span<const CellSlot> cells() const noexcept {
    return slots;
  }

  /// The spans @p ref's content is assembled from, in order. Empty for a cell
  /// with no content. The addresses are what a transclusion shares, so this is
  /// the honest answer to "what is this cell made of" -- textOf() is the
  /// convenience over it.
  [[nodiscard]] std::span<const xanadu::PrimediaSpan>
  contentOf(CellRef ref) const noexcept;

  /// The content of @p ref, read through @p reader.
  ///
  /// Answers std::string rather than the string_view the design sketched:
  /// SpanReader::read() returns by value, because content can come from a
  /// torrent rather than from memory this process already has mapped.
  [[nodiscard]] std::string textOf(CellRef ref,
                                   const xanadu::SpanReader &reader) const;

  /// The typed value of @p ref, when it carries bits of that kind -- and
  /// nothing when it carries another, rather than converting. A cell holding
  /// the integer 1 and a cell holding `true` render differently and mean
  /// different things, so reading one as the other is the caller's decision to
  /// state rather than this class's to make.
  ///
  /// Never parses the cell's text: the whole point of R6 carrying both halves
  /// is that a query reads the bits. textOf() is the other half.
  [[nodiscard]] std::optional<double> asDouble(CellRef ref) const noexcept;
  [[nodiscard]] std::optional<bool> asBool(CellRef ref) const noexcept;
  [[nodiscard]] std::optional<std::int64_t> asInt64(CellRef ref) const noexcept;

  /// Which of the above would answer, or ValueKind::None for a cell whose
  /// content is just content.
  [[nodiscard]] xanadu::ValueKind valueKindOf(CellRef ref) const noexcept;

  /**
   * @brief The group master of @p ref, following @p cloneDim negward.
   *
   * Takes the dimension rather than assuming one: d.clone is a cell like every
   * other dimension (R2), so there is no compiled-in ordinal to reach for.
   * dimensionNamed() is how a caller finds it by name.
   *
   * A rank that loops answers the cell it started from rather than spinning,
   * the same way zzcore's findCloneMaster() does.
   */
  [[nodiscard]] CellRef cloneMaster(CellRef ref,
                                    DimRef cloneDim) const noexcept;

  /**
   * @brief The dimensions @p ref links on -- d.meta-dims, read off the run.
   *
   * Exact and uncapped; the fixed array this replaced could only ever report
   * its first eight. The rank of ephemeral clone cells R12 describes is the
   * same information spelled in the manifold's own vocabulary, and is not
   * materialised here because nothing traverses it yet.
   */
  [[nodiscard]] std::span<const DimLink>
  dimensionsOf(CellRef ref) const noexcept;

  /// Every dimension in the slice: the d.dims rank, walked posward from home.
  [[nodiscard]] std::span<const DimRef> dimensions() const;

  /// The dimension whose content reads as @p name, or noCell. A linear walk of
  /// the d.dims rank, which is a dozen cells in a slice rather than a lookup
  /// worth indexing.
  [[nodiscard]] DimRef dimensionNamed(std::string_view name,
                                      const xanadu::SpanReader &reader) const;

  /// The two cells genesis mints by fiat: the first two cells folded, in the
  /// order Store::sliceGenesis() mints them. noCell in a store that never
  /// called it. See R5 and R12.
  [[nodiscard]] CellRef home() const noexcept { return home_; }
  [[nodiscard]] DimRef dimsDimension() const noexcept { return dimsDim_; }

  /// The highest operation index this fold has seen. What
  /// verifyAgainstFullRebuild() re-folds from.
  [[nodiscard]] std::uint32_t foldedThrough() const noexcept {
    return foldedThrough_;
  }

  /// Operations applyStructure() refused. Nonzero means the spool holds a
  /// Structure operation this manifold could not make sense of -- an unknown
  /// subject, an ephemeral target -- and the count is the only trace, since
  /// folding cannot throw.
  [[nodiscard]] std::uint32_t refusedOps() const noexcept {
    return refusedOps_;
  }

  // -- fold path: driven only by Store ---------------------------------------

  /**
   * @brief Fold one Structure operation in.
   *
   * Refuses, without throwing and without mutating anything, an operation it
   * cannot make sense of: a SetLink or SetValue whose subject chain does not
   * reach a cell this manifold holds, a second MakeCell at one index, or --
   * R8's boundary as a bit rather than a convention -- a link whose target or
   * dimension isEphemeral(). Each refusal bumps refusedOps().
   *
   * Anything that is not OpKind::Structure is ignored, so a caller may hand
   * over every node on a path without filtering first.
   */
  void applyStructure(std::uint32_t opIndex,
                      const xanadu::CompactOpNode &node) noexcept;

  /**
   * @brief Fold in the one operation that reaches @p version from the state
   *        this manifold already represents.
   *
   * The Manifold half of Store::advance(): incremental application is the
   * whole reason R9 tolerates a materialised view, and this is what a caller
   * that has just recorded a Structure operation calls instead of re-folding.
   *
   * @return false when @p version names no operation, which is the caller's
   *         cue to rebuild. The manifold is left untouched.
   */
  bool advance(const xanadu::Store &store,
               const xanadu::MicroversionId &version);

  /// Discard the arena's dead runs, leaving every cell's links contiguous in
  /// dense order. A full fold ends with one of these, so a freshly rebuilt
  /// manifold has a tight arena and the per-cell cost R12 quotes.
  void compact();

  // -- honesty ---------------------------------------------------------------

  /**
   * @brief Whether this incrementally-driven state equals a cold fold.
   *
   * Re-folds @p store from scratch through foldedThrough() and compares what
   * it means rather than how it is laid out: dense ids and arena offsets are
   * allowed to differ, cells and their links are not.
   *
   * This is the mechanism R9 puts in place of a promise. A view that is
   * cheaper than the thing it is a view of is a view that can drift, and the
   * only defence is to be able to ask.
   */
  [[nodiscard]] bool verifyAgainstFullRebuild(const xanadu::Store &store) const;

  /// Whether two manifolds hold the same cells with the same links, however
  /// their arenas are laid out.
  [[nodiscard]] bool equivalentTo(const Manifold &other) const;

private:
  /// The dense id for @p ref, or npos. Resolves chain indices as slot() does.
  [[nodiscard]] std::uint32_t denseOf(CellRef ref) const noexcept;

  /// @p cell's link to @p dim, appending an entry to its run if it has none.
  /// Returns nullptr only if the run cannot be grown.
  [[nodiscard]] DimLink *linkFor(std::uint32_t dense, DimRef dim);

  /// @p cell's existing link to @p dim, or nullptr.
  [[nodiscard]] DimLink *existingLink(std::uint32_t dense, DimRef dim) noexcept;

  void setOneSide(std::uint32_t dense, DimRef dim, DimVector dir, CellRef to);

  /// Replace @p dense's content run with @p spans, growing or relocating the
  /// arena as needed.
  void setContent(std::uint32_t dense,
                  std::span<const xanadu::PrimediaSpan> spans);

  /// Replace [@p at, @p at + @p removing) of @p dense's content with
  /// @p inserted, keeping the addresses of everything either side.
  void spliceContent(std::uint32_t dense, std::uint64_t at,
                     std::uint64_t removing,
                     const xanadu::PrimediaSpan &inserted);

  std::vector<CellSlot> slots;
  /// The CSR arena. A cell's run is grown in place when it is the arena's
  /// tail and relocated to the end otherwise, so the arena accumulates dead
  /// runs while a slice is being built and compact() reclaims them. There is
  /// no per-cell spare-capacity field on purpose: CellSlot is 48 bytes by
  /// assertion, and spending four of them on a capacity would have made the
  /// run design cost exactly what the fixed array it replaced cost (R12's
  /// 108 bytes per cell against 112), which is most of why the run won.
  std::vector<DimLink> links;
  /// The content arena, run per cell, grown and compacted exactly as @ref links
  /// is. Separate from the links because the two grow independently: a cell
  /// gains dimensions and gains text at different times, and interleaving them
  /// in one arena would relocate a run every time the other kind was appended.
  std::vector<xanadu::PrimediaSpan> content;
  std::size_t liveContent{0};
  /// Operation index -> dense id, for every operation in a cell's chain and
  /// not only for its birth op -- see slot().
  std::unordered_map<CellRef, std::uint32_t> byRef;
  /// Live entries in @ref links. The difference against links.size() is the
  /// dead runs compact() would reclaim.
  std::size_t liveLinks{0};

  CellRef home_{noCell};
  DimRef dimsDim_{noCell};
  std::uint32_t foldedThrough_{0};
  std::uint32_t refusedOps_{0};

  /// dimensions() is a rank walk, and a span has to point at something.
  mutable std::vector<DimRef> dimsCache;
  mutable bool dimsCacheStale{true};
};

} // namespace zigzag

#endif // ZIGZAG_MANIFOLD_HPP

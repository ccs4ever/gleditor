/**
 * @file sql_promotion.hpp
 * @brief The annotation crossing (design/sql-to-zigzag.md section 6): a
 * staging envelope in an arena overlaid on the annotation store's folded
 * manifold, the envelope dims declared lazily at first promote, the
 * d.promoted reconcile rank, and the ExternalKey -> durable-cell map.
 */
#ifndef COMMON_XANADU_ZIGZAG_SQL_PROMOTION_HPP
#define COMMON_XANADU_ZIGZAG_SQL_PROMOTION_HPP

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

class SqlLens;

/// Drives the two-arena promote gesture (spec section 6.1) for one
/// (lens, annotation store) pair. Owns the folded base view, the staging
/// overlay, the reconcile map, and the sql.promoted headcell.
///
/// Sequence per gesture: ensureReady (dims + headcell in the store) ->
/// staging.mark() -> buildEnvelope -> membership link on d.promoted ->
/// promote() -> staging.release() -> refold -> record the reconcile map.
class SqlPromotion {
public:
  SqlPromotion(SqlLens &lens, xanadu::Store &store);

  /// Mint the envelope dims and the sql.promoted:<dbid> headcell in the
  /// store if absent, then rebuild the reconcile map from the d.promoted
  /// rank. Idempotent; the laziness is deliberate -- a session that never
  /// promotes writes zero ops (spec section 6.1).
  [[nodiscard]] std::expected<void, std::string> ensureReady();

  /// Promote one subject (a row) or one page (its subjects in window
  /// order). Idempotent: a repeated promote of the same ExternalKey
  /// resolves to the existing durable twin -- the difference between
  /// "annotate the invoice" and "annotate a copy of the invoice".
  [[nodiscard]] std::expected<CellRef, std::string>
  promoteSubject(const ExternalKey &key);
  [[nodiscard]] std::expected<CellRef, std::string>
  promotePage(const ExternalKey &key);

  /// The durable twin @p key previously promoted to, if any.
  [[nodiscard]] std::optional<CellRef> twinOf(const ExternalKey &key) const {
    const auto it = reconcile_.find(key);
    return it == reconcile_.end() ? std::nullopt : std::optional{it->second};
  }

  /// The folded view of the store (refreshed after every write) -- where
  /// durable twins live. For tests and the rebuild walk.
  [[nodiscard]] const Manifold *base() const { return &base_; }

private:
  std::expected<CellRef, std::string> promoteImpl(const ExternalKey &key);
  /// Re-fold the store into base_ at the current head.
  std::expected<void, std::string> refold();
  /// Walk every d.extkey under @p env into the reconcile map.
  void recordReconcile(CellRef env);
  /// Re-derive the reconcile map from the d.promoted rank (post-ensureReady).
  void rebuildReconcile();

  SqlLens &lens_;
  xanadu::Store &store_;
  Manifold base_;                   ///< folded view of the store
  ArenaManifold staging_{&base_};   ///< envelope arena over base_
  xanadu::MicroversionId head_{};   ///< current store head (op parent)
  std::optional<CellRef> headCell_; ///< sql.promoted:<dbid> headcell
  bool ready_{false};
  /// ExternalKey -> the durable cell it was promoted to (spec 6.3).
  std::unordered_map<ExternalKey, CellRef> reconcile_;
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_SQL_PROMOTION_HPP

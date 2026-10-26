/**
 * @file sql_promotion.cpp
 * @brief The SqlPromotion gesture driver (design/sql-to-zigzag.md
 * section 6).
 */

#include "common/xanadu/zigzag/sql_promotion.hpp"

#include <vector>

#include "common/xanadu/zigzag/sql_lens.hpp"

namespace zigzag {

SqlPromotion::SqlPromotion(SqlLens &lens, xanadu::Store &store)
    : lens_(lens), store_(store) {}

std::expected<void, std::string> SqlPromotion::refold() {
  base_ = store_.rebuildManifold(head_);
  return {};
}

std::expected<void, std::string> SqlPromotion::ensureReady() {
  if (ready_) {
    return {};
  }
  if (!store_.homeCell()) {
    head_ = store_.sliceGenesis(xanadu::MicroversionId{}, "sql-annot");
  }
  if (auto r = refold(); !r) {
    return r;
  }
  // Envelope dims minted lazily in the target store -- after the refold
  // they resolve through the staging overlay to non-ephemeral refs that
  // promote() maps identically (spec section 6.1's two-fact constraint).
  for (const auto name : SqlLens::envelopeDims) {
    if (base_.dimensionNamed(name).has_value()) {
      continue;
    }
    head_ = store_.makeDimension(head_, name).version;
  }
  if (auto r = refold(); !r) {
    return r;
  }
  // The d.promoted headcell: one per dbid, a d.contains child of home so
  // reopen finds it by name from a known anchor (spec section 6.3).
  const std::string headName = "sql.promoted:" + lens_.dbid();
  const auto dContains       = base_.dimensionNamed("d.contains");
  const auto home            = store_.homeCell();
  const auto dSeq            = base_.dimensionNamed("d.seq");
  if (!home || !dContains || !dSeq) {
    return std::unexpected(
        "SQL promotion store is missing required dimensions");
  }
  for (auto at = base_.linked(*home, *dContains, Posward); at.has_value();) {
    if (base_.textOf(*at, store_) == headName) {
      headCell_ = *at;
      break;
    }
    at = base_.linked(*at, *dSeq, Posward);
  }
  if (headCell_ == noCell) {
    const auto v = store_.makeCell(head_, headName);
    head_        = v;
    headCell_    = store_.cellRefOf(v).value();
    head_        = store_.setLink(head_, *home, *dContains, Posward, headCell_);
  }
  if (auto r = refold(); !r) {
    return r;
  }
  rebuildReconcile();
  ready_ = true;
  return {};
}

void SqlPromotion::recordReconcile(CellRef env) {
  const auto dExtkey = base_.dimensionNamed("d.extkey");
  const auto dCont   = base_.dimensionNamed("d.contains");
  const auto dSeq    = base_.dimensionNamed("d.seq");
  if (!dExtkey || !dCont || !dSeq) {
    return;
  }
  // env -> contains -> subject twins -> contains -> value twins; every
  // cell carrying d.extkey joins the map keyed on that key's text.
  const auto walk = [&](CellRef from, auto &&walk) -> void {
    for (auto at = from; at != noCell;) {
      if (const auto a = base_.linked(at, *dExtkey, Posward); a.has_value()) {
        reconcile_[base_.textOf(*a, store_)] = at;
      }
      const auto first = base_.linked(at, *dCont, Posward);
      if (first.has_value()) {
        walk(*first, walk);
      }
      const auto nxt = base_.linked(at, *dSeq, Posward);
      at             = nxt.value_or(noCell);
    }
  };
  walk(env, walk);
}

void SqlPromotion::rebuildReconcile() {
  const auto dPromoted = base_.dimensionNamed("d.promoted");
  if (headCell_ == noCell || !dPromoted) {
    return;
  }
  // Members chain posward toward the headcell; read them negward from it.
  for (auto at = base_.linked(headCell_, *dPromoted, Negward);
       at.has_value();) {
    recordReconcile(*at);
    at = base_.linked(*at, *dPromoted, Negward);
  }
}

std::expected<CellRef, std::string>
SqlPromotion::promoteImpl(const ExternalKey &key) {
  if (const auto it = reconcile_.find(key); it != reconcile_.end()) {
    return it->second;
  }
  if (auto r = ensureReady(); !r) {
    return std::unexpected(r.error());
  }
  const auto guard = staging_.mark();
  auto env         = lens_.buildEnvelope(staging_, key);
  if (!env) {
    staging_.release(guard);
    return std::unexpected(env.error());
  }
  // The reconcile-rank membership rides inside the same microversion:
  // env -> headcell on d.promoted resolves to the base cell, which
  // promote() maps identically without expanding (spec section 6.3).
  const auto dPromoted = staging_.ensureDimension("d.promoted");
  (void)staging_.link(*env, dPromoted, Posward, headCell_);
  auto res = promote(store_, head_, staging_, *env);
  staging_.release(guard);
  if (!res.has_value()) {
    return std::unexpected("promote() refused the envelope");
  }
  head_ = res->version;
  if (auto r = refold(); !r) {
    return std::unexpected(r.error());
  }
  const CellRef durable = res->cells.front();
  recordReconcile(durable);
  return durable;
}

std::expected<CellRef, std::string>
SqlPromotion::promoteSubject(const ExternalKey &key) {
  return promoteImpl(key);
}

std::expected<CellRef, std::string>
SqlPromotion::promotePage(const ExternalKey &key) {
  return promoteImpl(key);
}

} // namespace zigzag

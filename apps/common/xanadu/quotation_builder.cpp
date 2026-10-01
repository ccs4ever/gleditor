/**
 * @file quotation_builder.cpp
 * @brief Interactive quotation builder model and preview engine (§5.10).
 */
#include "common/xanadu/quotation_builder.hpp"

#include <algorithm>
#include <unordered_set>

namespace xanadu {

void QuotationBuilder::setForeignStore(
    std::string scrollKey, const Store *store, const Scroll *sealedAs,
    std::optional<MicroversionId> pinnedVersion) {
  config_.foreignScrollKey = std::move(scrollKey);
  foreignStore_            = store;
  sealedAs_                = sealedAs;

  if (foreignStore_ != nullptr) {
    config_.pinnedVersion = pinnedVersion.value_or(foreignStore_->latest());
    foreignFold_          = std::make_shared<zigzag::Manifold>(
        foreignStore_->rebuildManifold(config_.pinnedVersion));

    if (config_.rootRef.produces.isZero() &&
        foreignStore_->homeCell() != zigzag::noCell) {
      setRootCell(foreignStore_->homeCell());
    }
  } else {
    foreignFold_.reset();
  }
}

void QuotationBuilder::setRootCell(const zigzag::CellRef cell) {
  if (nullptr == foreignStore_ || zigzag::noCell == cell) {
    return;
  }
  const auto op   = foreignStore_->segmentedOps().idOf(cell);
  config_.rootRef = ExternOpRef{.scroll = 1, .produces = op};
  if (foreignFold_ && foreignFold_->contains(cell)) {
    config_.rootLabel = foreignFold_->textOf(cell, *foreignStore_);
  }
}

void QuotationBuilder::setRootRef(ExternOpRef ref, std::string label) {
  config_.rootRef   = ref;
  config_.rootLabel = std::move(label);
}

void QuotationBuilder::setRankStep(const ExternOpRef dimRef,
                                   const zigzag::DimVector dir) {
  config_.rankDimRef    = dimRef;
  config_.rankDirection = dir;
}

void QuotationBuilder::setCarriedDimensions(
    std::vector<ExternOpRef> carryRefs) {
  config_.carryRefs = std::move(carryRefs);
}

void QuotationBuilder::addCarriedDimension(const ExternOpRef dimRef) {
  if (!std::ranges::contains(config_.carryRefs, dimRef)) {
    config_.carryRefs.push_back(dimRef);
  }
}

void QuotationBuilder::removeCarriedDimension(const ExternOpRef &dimRef) {
  std::erase(config_.carryRefs, dimRef);
}

void QuotationBuilder::setVqlQuery(std::string query,
                                   const std::uint32_t version) {
  config_.vqlQuery             = std::move(query);
  config_.queryLanguageVersion = version;
}

void QuotationBuilder::recomputePreview(const QuotationBudget &budget) {
  if (nullptr == foreignStore_ || nullptr == foreignFold_) {
    preview_.isValid       = false;
    preview_.state         = QuotationState::NotFetched;
    preview_.statusMessage = "Foreign document not loaded";
    preview_.cells.clear();
    return;
  }

  // Check foreign op path budget
  const auto opBytes = foreignStore_->opCount() * sizeof(CompactOpNode);
  if (budget.maxOpBytes > 0 && opBytes > budget.maxOpBytes) {
    preview_.isValid       = false;
    preview_.state         = QuotationState::TooLarge;
    preview_.statusMessage = "Foreign store exceeds op-path byte budget";
    preview_.cells.clear();
    return;
  }

  // Localise root ref
  const auto opIdx =
      foreignStore_->segmentedOps().indexOf(config_.rootRef.produces);
  if (opIdx == 0 || !foreignFold_->contains(opIdx)) {
    preview_.isValid = false;
    preview_.state   = QuotationState::Absent;
    preview_.statusMessage =
        "Root cell not found in foreign store at pinned version";
    preview_.cells.clear();
    return;
  }

  Selector selector;
  selector.kind                 = config_.mode;
  selector.root                 = opIdx;
  selector.rankDirection        = config_.rankDirection;
  selector.query                = config_.vqlQuery;
  selector.queryLanguageVersion = config_.queryLanguageVersion;
  selector.orderPolicy          = "identity";

  if (config_.mode == Selector::Kind::Rank && config_.rankDimRef) {
    const auto dIdx =
        foreignStore_->segmentedOps().indexOf(config_.rankDimRef->produces);
    if (dIdx != 0 && foreignFold_->contains(dIdx)) {
      selector.rankDim = dIdx;
    }
  } else if (config_.mode == Selector::Kind::Closure) {
    for (const auto &ref : config_.carryRefs) {
      const auto cIdx = foreignStore_->segmentedOps().indexOf(ref.produces);
      if (cIdx != 0 && foreignFold_->contains(cIdx)) {
        selector.carry.push_back(cIdx);
      }
    }
  }

  const auto selected = evaluateSelector(
      selector, *foreignFold_, *foreignStore_, budget, preview_.state);

  if (preview_.state != QuotationState::Resolved) {
    preview_.isValid = false;
    preview_.cells.clear();
    if (preview_.state == QuotationState::TooLarge) {
      preview_.statusMessage = "Quotation exceeds cell budget";
    } else if (preview_.state == QuotationState::Unintelligible) {
      preview_.statusMessage = "Unsupported VQL language version";
    } else {
      preview_.statusMessage = "Failed to evaluate selector";
    }
    return;
  }

  const std::unordered_set<zigzag::CellRef> selectedSet(selected.begin(),
                                                        selected.end());
  preview_.cells.clear();
  preview_.cells.reserve(selected.size());

  for (const auto fCell : selected) {
    PreviewCell pc;
    pc.foreignCell = fCell;
    pc.text        = foreignFold_->textOf(fCell, *foreignStore_);
    if (fCell == foreignFold_->home()) {
      pc.role = "home";
    } else if (std::ranges::contains(foreignFold_->dimensions(), fCell)) {
      pc.role = "dimension";
    }
    pc.birthRef = ExternOpRef{
        .scroll   = 1,
        .produces = foreignStore_->segmentedOps().idOf(fCell),
    };

    for (const auto &dimLink : foreignFold_->dimensionsOf(fCell)) {
      const auto nbr = dimLink.neighbor(zigzag::DimVector::POS);
      if (nbr != zigzag::noCell && selectedSet.contains(nbr)) {
        const auto dimName = foreignFold_->textOf(dimLink.dim, *foreignStore_);
        pc.outboundEdges.emplace_back(dimName, nbr);
      }
    }
    preview_.cells.push_back(std::move(pc));
  }

  preview_.isValid = true;
  preview_.statusMessage =
      "Resolved " + std::to_string(preview_.cells.size()) + " cell(s)";
  preview_.totalOpBytes = opBytes;
  preview_.totalProxies = static_cast<std::uint32_t>(preview_.cells.size());
  if (preview_.focusedIndex >= preview_.cells.size()) {
    preview_.focusedIndex = 0;
  }
}

void QuotationBuilder::focusPreviewCell(const std::size_t index) noexcept {
  if (index < preview_.cells.size()) {
    preview_.focusedIndex = index;
  }
}

void QuotationBuilder::navigatePreview(const int delta) noexcept {
  if (preview_.cells.empty()) {
    return;
  }
  const auto count = static_cast<int>(preview_.cells.size());
  int cur          = static_cast<int>(preview_.focusedIndex);
  cur              = (cur + delta) % count;
  if (cur < 0) {
    cur += count;
  }
  preview_.focusedIndex = static_cast<std::size_t>(cur);
}

AppendedQuotation QuotationBuilder::commit(Store &localStore,
                                           const MicroversionId &parent,
                                           const zigzag::CellRef localTail,
                                           const zigzag::DimRef localDim) {
  SelectorSpec spec;
  spec.kind                 = config_.mode;
  spec.rootRef              = config_.rootRef;
  spec.rankDimRef           = config_.rankDimRef;
  spec.rankDirection        = config_.rankDirection;
  spec.carryRefs            = config_.carryRefs;
  spec.query                = config_.vqlQuery;
  spec.queryLanguageVersion = config_.queryLanguageVersion;
  spec.orderPolicy          = "identity";

  auto commitParent = parent;
  if (!config_.foreignScrollKey.empty() &&
      !localStore.scrollRegistry().scrollIdForKey(config_.foreignScrollKey)) {
    commitParent = localStore.registerScroll(parent, config_.foreignScrollKey);
  }
  if (!config_.foreignScrollKey.empty()) {
    if (const auto sid = localStore.scrollRegistry().scrollIdForKey(
            config_.foreignScrollKey)) {
      spec.rootRef.scroll = *sid;
      if (spec.rankDimRef) {
        spec.rankDimRef->scroll = *sid;
      }
      for (auto &c : spec.carryRefs) {
        c.scroll = *sid;
      }
    }
  }

  const GlobalDocumentState pin{
      .scroll  = config_.foreignScrollKey,
      .version = config_.pinnedVersion,
  };

  return localStore.quote(commitParent, localTail, localDim,
                          config_.quotationLabel, pin, spec);
}

} // namespace xanadu

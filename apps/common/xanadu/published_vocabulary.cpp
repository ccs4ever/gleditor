/**
 * @file published_vocabulary.cpp
 * @brief Implementation of published vocabularies and dimension identity
 * (§5.11).
 */
#include "common/xanadu/published_vocabulary.hpp"

#include <algorithm>
#include <stdexcept>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {

VocabularyReleaseResult
publishVocabularyRelease(Store &store, const MicroversionId &parent,
                         zigzag::CellRef releaseCell,
                         const std::string_view releaseLabel,
                         const std::span<const zigzag::CellRef> terms) {
  auto curHead = parent.isZero() ? store.latest() : parent;
  std::optional<zigzag::Manifold> folded;
  auto currentFold = store.rebuildManifold(curHead);

  auto ensureDim = [&](const std::string_view name) -> zigzag::DimRef {
    auto dim = currentFold.dimensionNamed(name, store);
    if (!dim) {
      const auto minted = store.makeDimension(curHead, name, &currentFold);
      curHead           = minted.version;
      currentFold       = store.rebuildManifold(curHead);
      return minted.dim;
    }
    return *dim;
  };

  const auto dimDims = ensureDim(kDimDims);

  if (releaseCell == zigzag::noCell) {
    curHead     = store.makeCell(curHead, releaseLabel);
    releaseCell = store.cellRefOf(curHead);
    currentFold = store.rebuildManifold(curHead);

    auto tail = store.homeCell();
    for (auto step = currentFold.cellCount() + 1; step > 0; step--) {
      const auto next =
          currentFold.linked(tail, dimDims, zigzag::DimVector::POS);
      if (zigzag::noCell == next || next == releaseCell ||
          next == store.homeCell()) {
        break;
      }
      tail = next;
    }
    curHead     = store.setLink(curHead, tail, dimDims, zigzag::DimVector::POS,
                                releaseCell, &currentFold);
    currentFold = store.rebuildManifold(curHead);
  }

  // Link terms posward from releaseCell along d.dims
  auto prevCell = releaseCell;
  for (const auto term : terms) {
    if (term == zigzag::noCell || term == prevCell) {
      continue;
    }
    curHead = store.setLink(curHead, prevCell, dimDims, zigzag::DimVector::POS,
                            term, &currentFold);
    currentFold = store.rebuildManifold(curHead);
    prevCell    = term;
  }

  // Touch releaseCell last to pin publication state (§5.11 §3)
  curHead = store.setCellText(curHead, releaseCell, releaseLabel, &currentFold);
  currentFold = store.rebuildManifold(curHead);

  std::string sKey = store.bootstrapPermascrollKey();
  if (sKey.empty()) {
    sKey = "urn:xanadu:vocab:" +
           std::to_string(reinterpret_cast<std::uintptr_t>(&store));
  }

  const auto relSlot  = currentFold.slot(releaseCell);
  const auto dimsSlot = currentFold.slot(dimDims);
  if (!relSlot || !dimsSlot) {
    throw std::runtime_error("failed to locate release or dims cell slot");
  }
  const auto relBirth  = store.segmentedOps().idOf(relSlot->birthOp);
  const auto dimsBirth = store.segmentedOps().idOf(dimsSlot->birthOp);

  PublishedVocabulary vocab{
      .releaseCell = GlobalOpRef{.scroll = sKey, .produces = relBirth},
      .state       = GlobalDocumentState{.scroll = sKey, .version = curHead},
      .dimensions  = GlobalOpRef{.scroll = sKey, .produces = dimsBirth},
  };

  return VocabularyReleaseResult{.version     = curHead,
                                 .vocabulary  = std::move(vocab),
                                 .releaseCell = releaseCell};
}

AppendedQuotation adoptVocabulary(Store &localStore,
                                  const MicroversionId &parent,
                                  const PublishedVocabulary &vocab,
                                  const std::string_view localLabel,
                                  const zigzag::CellRef localRankTail,
                                  const zigzag::DimRef localRankDim) {
  auto curHead = parent.isZero() ? localStore.latest() : parent;
  if (!localStore.scrollRegistry().scrollIdForKey(vocab.state.scroll)) {
    curHead = localStore.registerScroll(curHead, vocab.state.scroll);
  }

  const auto scrollIdOpt =
      localStore.scrollRegistry().scrollIdForKey(vocab.state.scroll);
  if (!scrollIdOpt) {
    throw std::runtime_error("failed to register foreign vocabulary scroll: " +
                             vocab.state.scroll);
  }
  const auto scrollId = *scrollIdOpt;

  SelectorSpec selector{
      .kind          = Selector::Kind::Rank,
      .rootRef       = ExternOpRef{.scroll   = scrollId,
                                   .produces = vocab.releaseCell.produces},
      .rankDimRef    = ExternOpRef{.scroll   = scrollId,
                                   .produces = vocab.dimensions.produces},
      .rankDirection = zigzag::DimVector::POS,
  };

  auto fold               = localStore.rebuildManifold(curHead);
  zigzag::DimRef quoteDim = localRankDim;
  if (quoteDim == zigzag::noCell) {
    auto dimVocabOpt = fold.dimensionNamed(kDimVocab, localStore);
    if (dimVocabOpt) {
      quoteDim = *dimVocabOpt;
    } else {
      const auto minted = localStore.makeDimension(curHead, kDimVocab, &fold);
      curHead           = minted.version;
      quoteDim          = minted.dim;
      fold              = localStore.rebuildManifold(curHead);
    }
  }
  const zigzag::CellRef quoteTail =
      localRankTail != zigzag::noCell ? localRankTail : localStore.homeCell();

  return localStore.quote(curHead, quoteTail, quoteDim, localLabel, vocab.state,
                          selector, &fold);
}

AdoptedDimension adoptPublishedDimension(Store &store,
                                         const MicroversionId &parent,
                                         const GlobalOpRef &term,
                                         const std::string_view localAlias) {
  auto curHead = parent.isZero() ? store.latest() : parent;
  if (const auto existing = findAdoptedDimension(store, term, curHead);
      existing.has_value()) {
    if (!localAlias.empty()) {
      zigzag::DimensionRegistry::instance().registerDim(store, localAlias,
                                                        *existing);
    }
    return AdoptedDimension{.version = curHead, .local = *existing};
  }

  auto fold     = store.rebuildManifold(curHead);
  auto registry = fold.scrollRegistry(store);
  auto sidOpt   = registry.scrollIdForKey(term.scroll);
  if (!sidOpt.has_value()) {
    curHead  = store.registerScroll(curHead, term.scroll, &fold);
    fold     = store.rebuildManifold(curHead);
    registry = fold.scrollRegistry(store);
    sidOpt   = registry.scrollIdForKey(term.scroll);
  }
  if (!sidOpt.has_value()) {
    throw std::runtime_error(
        "failed to resolve scroll id for published term: " + term.scroll);
  }

  const ExternOpRef extRef{.scroll = *sidOpt, .produces = term.produces};
  curHead  = store.makeExternRef(curHead, extRef, &fold);
  fold     = store.rebuildManifold(curHead);
  registry = fold.scrollRegistry(store);

  const auto phOpt = registry.placeholderForExtern(extRef);
  if (!phOpt.has_value() || *phOpt == zigzag::noCell) {
    throw std::runtime_error(
        "failed to mint extern placeholder for published term");
  }
  const auto placeholder = *phOpt;

  // Link placeholder to local d.dims
  auto dimDimsOpt = fold.dimensionNamed(kDimDims, store);
  const zigzag::DimRef dimDims =
      dimDimsOpt ? *dimDimsOpt : store.dimsDimension();
  auto tail = store.homeCell();
  for (auto step = fold.cellCount() + 1; step > 0; step--) {
    const auto next = fold.linked(tail, dimDims, zigzag::DimVector::POS);
    if (zigzag::noCell == next || next == placeholder ||
        next == store.homeCell()) {
      break;
    }
    tail = next;
  }
  curHead = store.setLink(curHead, tail, dimDims, zigzag::DimVector::POS,
                          placeholder, &fold);
  fold    = store.rebuildManifold(curHead);

  if (!localAlias.empty()) {
    auto dimAliasOpt = fold.dimensionNamed("d.alias", store);
    zigzag::DimRef dimAlias;
    if (dimAliasOpt) {
      dimAlias = *dimAliasOpt;
    } else {
      const auto minted = store.makeDimension(curHead, "d.alias", &fold);
      curHead           = minted.version;
      dimAlias          = minted.dim;
      fold              = store.rebuildManifold(curHead);
    }
    const auto aliasOp  = store.makeCell(curHead, localAlias);
    const auto aliasRef = store.cellRefOf(aliasOp);
    curHead             = aliasOp;
    fold                = store.rebuildManifold(curHead);
    curHead = store.setLink(curHead, placeholder, dimAlias,
                            zigzag::DimVector::POS, aliasRef, &fold);
    zigzag::DimensionRegistry::instance().registerDim(store, localAlias,
                                                      placeholder);
  }

  return AdoptedDimension{.version = curHead, .local = placeholder};
}

std::optional<zigzag::DimRef>
findAdoptedDimension(const Store &store, const GlobalOpRef &term,
                     const MicroversionId &version) {
  const auto v        = version.isZero() ? store.latest() : version;
  const auto fold     = store.rebuildManifold(v);
  const auto registry = fold.scrollRegistry(store);
  const auto sidOpt   = registry.scrollIdForKey(term.scroll);
  if (!sidOpt.has_value()) {
    return std::nullopt;
  }
  const auto phOpt = registry.placeholderForExtern(
      ExternOpRef{.scroll = *sidOpt, .produces = term.produces});
  if (!phOpt.has_value() || *phOpt == zigzag::noCell) {
    return std::nullopt;
  }
  const auto dims = fold.dimensions();
  if (std::ranges::find(dims, *phOpt) != dims.end()) {
    return *phOpt;
  }
  return std::nullopt;
}

MicroversionId updateAdoptedVocabulary(Store &localStore,
                                       const MicroversionId &parent,
                                       const zigzag::CellRef quotationCell,
                                       const PublishedVocabulary &newVocab) {
  auto curHead    = parent.isZero() ? localStore.latest() : parent;
  auto fold       = localStore.rebuildManifold(curHead);
  const auto qOpt = readQuotation(fold, localStore, quotationCell);
  if (!qOpt.has_value()) {
    throw std::runtime_error("quotation cell not found or invalid");
  }

  if (!localStore.scrollRegistry().scrollIdForKey(newVocab.state.scroll)) {
    curHead = localStore.registerScroll(curHead, newVocab.state.scroll, &fold);
    fold    = localStore.rebuildManifold(curHead);
  }
  const auto sidOpt =
      localStore.scrollRegistry().scrollIdForKey(newVocab.state.scroll);
  if (!sidOpt) {
    throw std::runtime_error("failed to resolve scroll id for new vocabulary");
  }
  const auto sid = *sidOpt;

  // Mint new state descriptor
  const auto stateDesc    = writeGlobalDocumentState(newVocab.state);
  curHead                 = localStore.makeCell(curHead, stateDesc);
  const auto newStateCell = localStore.cellRefOf(curHead);
  fold                    = localStore.rebuildManifold(curHead);

  // Mint new selector descriptor
  SelectorSpec newSpec = qOpt->selectorSpec;
  newSpec.rootRef =
      ExternOpRef{.scroll = sid, .produces = newVocab.releaseCell.produces};
  newSpec.rankDimRef =
      ExternOpRef{.scroll = sid, .produces = newVocab.dimensions.produces};
  newSpec.rankDirection = zigzag::DimVector::POS;

  const auto selDesc    = writeSelectorDescriptor(newSpec);
  curHead               = localStore.makeCell(curHead, selDesc);
  const auto newSelCell = localStore.cellRefOf(curHead);
  fold                  = localStore.rebuildManifold(curHead);

  const auto dimQuotes = *fold.dimensionNamed(kDimQuotes, localStore);
  const auto dimState  = *fold.dimensionNamed(kDimQuotesState, localStore);
  const auto dimSel    = *fold.dimensionNamed(kDimQuotesSel, localStore);

  // Link rank dimension placeholder from selector cell if needed
  if (newSpec.rankDimRef.has_value()) {
    curHead = localStore.makeExternRef(curHead, *newSpec.rankDimRef, &fold);
    fold    = localStore.rebuildManifold(curHead);
    const auto dimPh = fold.scrollRegistry(localStore)
                           .placeholderForExtern(*newSpec.rankDimRef);
    if (dimPh && *dimPh != zigzag::noCell) {
      curHead = localStore.setLink(curHead, newSelCell, dimQuotes,
                                   zigzag::DimVector::POS, *dimPh, &fold);
      fold    = localStore.rebuildManifold(curHead);
    }
  }

  // Repoint quotation cell to new state cell and new selector cell
  curHead = localStore.setLink(curHead, quotationCell, dimState,
                               zigzag::DimVector::POS, newStateCell, &fold);
  fold    = localStore.rebuildManifold(curHead);

  curHead = localStore.setLink(curHead, quotationCell, dimSel,
                               zigzag::DimVector::POS, newSelCell, &fold);
  return curHead;
}

} // namespace xanadu

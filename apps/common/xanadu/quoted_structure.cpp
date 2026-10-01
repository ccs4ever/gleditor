/**
 * @file quoted_structure.cpp
 * @brief Implementation of quoted structure and selector resolution.
 */
#include "common/xanadu/quoted_structure.hpp"

#include <algorithm>
#include <charconv>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

#include "common/xanadu/publication.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace xanadu {

std::string writeSelectorDescriptor(const SelectorSpec &spec) {
  std::string out;
  switch (spec.kind) {
  case Selector::Kind::Cell:
    out += "kind:cell\n";
    break;
  case Selector::Kind::Rank:
    out += "kind:rank\n";
    out += "dir:";
    out += (spec.rankDirection == zigzag::DimVector::NEG ? "neg\n" : "pos\n");
    break;
  case Selector::Kind::Closure:
    out += "kind:closure\n";
    break;
  case Selector::Kind::Query:
    out += "kind:query\n";
    out += "version:" + std::to_string(spec.queryLanguageVersion) + "\n";
    out +=
        "order:" + (spec.orderPolicy.empty() ? "identity" : spec.orderPolicy) +
        "\n";
    out += "query:" + spec.query + "\n";
    break;
  }
  return out;
}

std::optional<SelectorSpec>
readSelectorDescriptor(const std::string_view text) {
  SelectorSpec spec;
  std::istringstream stream{std::string(text)};
  std::string line;
  bool hasKind = false;

  while (std::getline(stream, line)) {
    if (line.empty() || line.back() == '\r') {
      if (!line.empty()) {
        line.pop_back();
      }
    }
    if (line.starts_with("kind:")) {
      const auto k = line.substr(5);
      if (k == "cell") {
        spec.kind = Selector::Kind::Cell;
      } else if (k == "rank") {
        spec.kind = Selector::Kind::Rank;
      } else if (k == "closure") {
        spec.kind = Selector::Kind::Closure;
      } else if (k == "query") {
        spec.kind = Selector::Kind::Query;
      } else {
        return std::nullopt;
      }
      hasKind = true;
    } else if (line.starts_with("dir:")) {
      const auto d = line.substr(4);
      spec.rankDirection =
          (d == "neg") ? zigzag::DimVector::NEG : zigzag::DimVector::POS;
    } else if (line.starts_with("version:")) {
      const auto v      = line.substr(8);
      std::uint32_t ver = 0;
      auto [p, ec]      = std::from_chars(v.data(), v.data() + v.size(), ver);
      if (ec == std::errc{}) {
        spec.queryLanguageVersion = ver;
      }
    } else if (line.starts_with("order:")) {
      spec.orderPolicy = line.substr(6);
    } else if (line.starts_with("query:")) {
      spec.query = line.substr(6);
      std::string rest;
      while (std::getline(stream, rest)) {
        if (!rest.empty() && rest.back() == '\r') {
          rest.pop_back();
        }
        spec.query += "\n" + rest;
      }
      break;
    }
  }

  if (!hasKind) {
    return std::nullopt;
  }
  return spec;
}

void InMemoryForeignSource::registerDocument(const GlobalDocumentState &state,
                                             const Store *store,
                                             const Scroll *sealedAs,
                                             const FetchState fetchState) {
  const auto key = state.scroll + "@" + state.version.str();
  entries_[key] =
      Entry{.store = store, .sealedAs = sealedAs, .state = fetchState};
}

void InMemoryForeignSource::setFetchState(const GlobalDocumentState &state,
                                          const FetchState fetchState) {
  const auto key = state.scroll + "@" + state.version.str();
  if (auto it = entries_.find(key); it != entries_.end()) {
    it->second.state = fetchState;
  } else {
    entries_[key] =
        Entry{.store = nullptr, .sealedAs = nullptr, .state = fetchState};
  }
}

void InMemoryForeignSource::requestDocument(
    const ForeignRequest &req,
    gleditor::cpp26::function_ref<void(FetchState, ForeignDocument)> callback) {
  if (req.cancellation.isCancelled()) {
    callback(FetchState::NotFetched, ForeignDocument{});
    return;
  }
  const auto key = req.state.scroll + "@" + req.state.version.str();
  const auto it  = entries_.find(key);
  if (it == entries_.end()) {
    callback(FetchState::Absent, ForeignDocument{});
    return;
  }
  callback(it->second.state, ForeignDocument{.store    = it->second.store,
                                             .sealedAs = it->second.sealedAs});
}

std::optional<Quotation> readQuotation(const zigzag::Manifold &manifold,
                                       const Store &store,
                                       const zigzag::CellRef qCell) {
  if (qCell == zigzag::noCell || !manifold.contains(qCell)) {
    return std::nullopt;
  }

  const auto dimQuotesOpt = manifold.dimensionNamed(kDimQuotes, store);
  const auto dimQuotesStateOpt =
      manifold.dimensionNamed(kDimQuotesState, store);
  const auto dimQuotesSelOpt = manifold.dimensionNamed(kDimQuotesSel, store);

  if (!dimQuotesOpt || !dimQuotesStateOpt || !dimQuotesSelOpt) {
    return std::nullopt;
  }

  std::optional<zigzag::CellRef> placeholderOpt;
  zigzag::walkRank(manifold, qCell, *dimQuotesOpt, zigzag::DimVector::POS,
                   [&](const zigzag::CellRef c) {
                     if (c != qCell && store.externTarget(c).has_value()) {
                       placeholderOpt = c;
                       return false;
                     }
                     return true;
                   });
  const auto stateCellOpt = zigzag::step(manifold, qCell, *dimQuotesStateOpt);
  const auto selCellOpt   = zigzag::step(manifold, qCell, *dimQuotesSelOpt);

  if (!placeholderOpt || !stateCellOpt || !selCellOpt) {
    return std::nullopt;
  }

  const auto stateText = manifold.textOf(*stateCellOpt, store);
  const auto stateOpt  = readGlobalDocumentState(stateText);
  if (!stateOpt) {
    return std::nullopt;
  }

  const auto selText = manifold.textOf(*selCellOpt, store);
  auto selSpecOpt    = readSelectorDescriptor(selText);
  if (!selSpecOpt) {
    return std::nullopt;
  }

  const auto rootOpOpt = store.externTarget(*placeholderOpt);
  if (!rootOpOpt) {
    return std::nullopt;
  }
  selSpecOpt->rootRef = *rootOpOpt;

  if (selSpecOpt->kind == Selector::Kind::Rank) {
    std::optional<zigzag::CellRef> dimPhOpt;
    zigzag::walkRank(manifold, *selCellOpt, *dimQuotesOpt,
                     zigzag::DimVector::POS, [&](const zigzag::CellRef c) {
                       if (c != *selCellOpt &&
                           store.externTarget(c).has_value()) {
                         dimPhOpt = c;
                         return false;
                       }
                       return true;
                     });
    if (dimPhOpt) {
      selSpecOpt->rankDimRef = store.externTarget(*dimPhOpt);
    }
  } else if (selSpecOpt->kind == Selector::Kind::Closure) {
    const auto dimQuotesCarryOpt =
        manifold.dimensionNamed(kDimQuotesCarry, store);
    if (dimQuotesCarryOpt) {
      selSpecOpt->carryRefs.clear();
      zigzag::walkRank(manifold, *selCellOpt, *dimQuotesCarryOpt,
                       zigzag::DimVector::POS, [&](const zigzag::CellRef c) {
                         if (c != *selCellOpt) {
                           if (const auto ext = store.externTarget(c)) {
                             selSpecOpt->carryRefs.push_back(*ext);
                           }
                         }
                         return true;
                       });
    }
  }

  std::vector<OverrideEntry> overrides;
  if (const auto dimOverridesOpt =
          manifold.dimensionNamed(kDimOverrides, store)) {
    if (const auto dimShadowsOpt =
            manifold.dimensionNamed(kDimShadows, store)) {
      zigzag::walkRank(
          manifold, qCell, *dimOverridesOpt, zigzag::DimVector::POS,
          [&](const zigzag::CellRef oCell) {
            if (oCell != qCell) {
              if (const auto targetPh =
                      zigzag::step(manifold, oCell, *dimShadowsOpt)) {
                if (const auto targetExt = store.externTarget(*targetPh)) {
                  const auto scrollRec =
                      store.scrollRegistry().findRecord(targetExt->scroll);
                  GlobalOpRef gRef{
                      .scroll   = scrollRec ? scrollRec->globalKey : "",
                      .produces = targetExt->produces,
                  };
                  overrides.push_back(OverrideEntry{
                      .overrideCell    = oCell,
                      .placeholderCell = *targetPh,
                      .targetGlobalRef = std::move(gRef),
                  });
                }
              }
            }
            return true;
          });
    }
  }

  return Quotation{
      .head            = qCell,
      .placeholderCell = *placeholderOpt,
      .stateCell       = *stateCellOpt,
      .selectorCell    = *selCellOpt,
      .pinnedState     = *stateOpt,
      .selectorSpec    = std::move(*selSpecOpt),
      .overrides       = std::move(overrides),
      .label           = manifold.textOf(qCell, store),
  };
}

std::vector<zigzag::CellRef>
evaluateSelector(const Selector &selector,
                 const zigzag::Manifold &foreignManifold,
                 const Store &foreignStore, const QuotationBudget &budget,
                 QuotationState &outState) {
  outState = QuotationState::Resolved;
  std::vector<zigzag::CellRef> results;

  switch (selector.kind) {
  case Selector::Kind::Cell: {
    if (selector.root != zigzag::noCell &&
        foreignManifold.contains(selector.root)) {
      results.push_back(selector.root);
    }
    break;
  }

  case Selector::Kind::Rank: {
    if (selector.root == zigzag::noCell ||
        !foreignManifold.contains(selector.root) ||
        selector.rankDim == zigzag::noCell) {
      break;
    }
    zigzag::CellRef cur = selector.root;
    std::unordered_set<zigzag::CellRef> visited;

    while (cur != zigzag::noCell) {
      if (visited.contains(cur)) {
        break; // Ring stop before revisiting start
      }
      visited.insert(cur);
      results.push_back(cur);
      if (results.size() >= budget.maxCells) {
        outState = QuotationState::TooLarge;
        return results;
      }
      cur =
          foreignManifold.linked(cur, selector.rankDim, selector.rankDirection);
    }
    break;
  }

  case Selector::Kind::Closure: {
    if (selector.root == zigzag::noCell ||
        !foreignManifold.contains(selector.root)) {
      break;
    }

    std::queue<zigzag::CellRef> q;
    std::unordered_set<zigzag::CellRef> visited;

    q.push(selector.root);
    visited.insert(selector.root);

    while (!q.empty()) {
      const auto u = q.front();
      q.pop();
      results.push_back(u);

      if (results.size() >= budget.maxCells) {
        outState = QuotationState::TooLarge;
        return results;
      }

      for (const auto dim : selector.carry) {
        for (const auto dir :
             {zigzag::DimVector::POS, zigzag::DimVector::NEG}) {
          const auto v = foreignManifold.linked(u, dim, dir);
          if (v != zigzag::noCell && foreignManifold.contains(v) &&
              !visited.contains(v)) {
            visited.insert(v);
            q.push(v);
          }
        }
      }
    }
    break;
  }

  case Selector::Kind::Query: {
    if (selector.queryLanguageVersion != kSupportedVqlLanguageVersion) {
      outState = QuotationState::Unintelligible;
      return {};
    }

    try {
      zigzag::ArenaManifold foreignArena(&foreignManifold, &foreignStore);
      vql::VQLEngine engine(foreignArena);
      results = engine.execute(selector.query);

      if (results.size() >= budget.maxCells) {
        outState = QuotationState::TooLarge;
        return results;
      }

      // Canonical sort if requested
      if (selector.orderPolicy == "identity") {
        std::ranges::sort(
            results, [&](const zigzag::CellRef a, const zigzag::CellRef b) {
              const auto aSlot = foreignManifold.slot(a);
              const auto bSlot = foreignManifold.slot(b);
              const auto aOp   = aSlot ? aSlot->birthOp : a;
              const auto bOp   = bSlot ? bSlot->birthOp : b;
              return aOp < bOp;
            });
      }
    } catch (...) {
      outState = QuotationState::Unintelligible;
      return {};
    }
    break;
  }
  }

  return results;
}

void resolveQuotations(zigzag::ArenaManifold &arena,
                       const zigzag::Manifold &base, const Store &localStore,
                       IForeignSource &source, const QuotationBudget budget,
                       const std::uint32_t viewEpoch) {
  (void)viewEpoch;
  const auto dimQuotes      = base.dimensionNamed(kDimQuotes, localStore);
  const auto dimQuotesState = base.dimensionNamed(kDimQuotesState, localStore);
  if (!dimQuotes || !dimQuotesState) {
    return;
  }

  // Find all reachable quotation heads across base
  std::vector<zigzag::CellRef> quoteHeads;
  for (const auto &cell : base.cells()) {
    const auto qCell = cell.birthOp;
    if (base.linked(qCell, *dimQuotesState, zigzag::DimVector::POS) !=
        zigzag::noCell) {
      quoteHeads.push_back(qCell);
    }
  }

  std::unordered_set<zigzag::CellRef> visitedQuotes;
  static std::atomic<zigzag::QuoteViewId> sViewCounter{1};

  for (const auto qCell : quoteHeads) {
    if (visitedQuotes.contains(qCell)) {
      continue;
    }
    visitedQuotes.insert(qCell);

    const auto quotationOpt = readQuotation(base, localStore, qCell);
    if (!quotationOpt) {
      continue;
    }
    const auto &q = *quotationOpt;

    ForeignRequest req{
        .state        = q.pinnedState,
        .cancellation = CancellationToken{},
    };

    FetchState fetchState = FetchState::NotFetched;
    ForeignDocument foreignDoc;

    source.requestDocument(req,
                           [&](const FetchState s, const ForeignDocument doc) {
                             fetchState = s;
                             foreignDoc = doc;
                           });

    if (fetchState != FetchState::Ready || nullptr == foreignDoc.store) {
      continue;
    }

    // Preselection budget check: op path size
    if (foreignDoc.store->opCount() * sizeof(CompactOpNode) >
        budget.maxOpBytes) {
      continue;
    }

    // Fold foreign store up to pinnedState.version
    auto foreignFold = std::make_shared<zigzag::Manifold>(
        foreignDoc.store->rebuildManifold(q.pinnedState.version));

    auto localiseExtern =
        [&](const ExternOpRef &ext) -> std::optional<zigzag::CellRef> {
      const auto scrollRec = localStore.scrollRegistry().findRecord(ext.scroll);
      const std::string scrollKey = (scrollRec && !scrollRec->globalKey.empty())
                                        ? scrollRec->globalKey
                                        : q.pinnedState.scroll;
      const GlobalOpRef gRef{.scroll = scrollKey, .produces = ext.produces};

      if (foreignDoc.sealedAs != nullptr) {
        const auto opIdx =
            localiseOpRef(*foreignDoc.store, gRef, *foreignDoc.sealedAs);
        if (opIdx && foreignFold->contains(*opIdx)) {
          return *opIdx;
        }
      }
      const auto opIdx = foreignDoc.store->segmentedOps().indexOf(ext.produces);
      if (opIdx != 0 && foreignFold->contains(opIdx)) {
        return opIdx;
      }
      return std::nullopt;
    };

    // Localise selector references into foreign fold
    Selector selector;
    selector.kind                 = q.selectorSpec.kind;
    selector.rankDirection        = q.selectorSpec.rankDirection;
    selector.query                = q.selectorSpec.query;
    selector.queryLanguageVersion = q.selectorSpec.queryLanguageVersion;
    selector.orderPolicy          = q.selectorSpec.orderPolicy;

    selector.root =
        localiseExtern(q.selectorSpec.rootRef).value_or(zigzag::noCell);

    if (q.selectorSpec.kind == Selector::Kind::Rank &&
        q.selectorSpec.rankDimRef.has_value()) {
      selector.rankDim =
          localiseExtern(*q.selectorSpec.rankDimRef).value_or(zigzag::noCell);
    } else if (q.selectorSpec.kind == Selector::Kind::Closure) {
      for (const auto &ref : q.selectorSpec.carryRefs) {
        if (const auto localized = localiseExtern(ref)) {
          selector.carry.push_back(*localized);
        }
      }
    }

    QuotationState qState = QuotationState::Resolved;
    const auto selected   = evaluateSelector(selector, *foreignFold,
                                             *foreignDoc.store, budget, qState);

    if (qState != QuotationState::Resolved || selected.empty()) {
      continue;
    }

    // Map overrides by GlobalOpRef
    std::map<GlobalOpRef, zigzag::CellRef> overrideMap;
    for (const auto &ov : q.overrides) {
      overrideMap[ov.targetGlobalRef] = ov.overrideCell;
    }

    const auto spaceId = arena.attach(zigzag::Space{
        .manifold      = foreignFold.get(),
        .ownedManifold = foreignFold,
        .store         = foreignDoc.store,
        .sealedAs      = foreignDoc.sealedAs,
        .reader        = foreignDoc.store,
        .label         = "quoted_" + q.pinnedState.scroll,
    });

    const auto viewId = sViewCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string foreignScrollStr = q.pinnedState.scroll;

    std::unordered_map<zigzag::CellRef, zigzag::CellRef> foreignToView;
    std::vector<zigzag::CellRef> viewRun;
    viewRun.reserve(selected.size());

    for (const auto fCell : selected) {
      const auto fSlot   = foreignFold->slot(fCell);
      const auto birthOp = fSlot ? fSlot->birthOp : fCell;
      GlobalOpRef gRef{
          .scroll   = foreignScrollStr,
          .produces = foreignDoc.store->segmentedOps().idOf(birthOp),
      };

      if (const auto it = overrideMap.find(gRef); it != overrideMap.end()) {
        foreignToView[fCell] = it->second;
        viewRun.push_back(it->second);
      } else {
        const auto proxy     = arena.proxyFor(spaceId, fCell);
        const auto occ       = arena.quoteOccurrence(viewId, proxy);
        foreignToView[fCell] = occ;
        viewRun.push_back(occ);
      }
    }

    // Link induced edges among selected cells
    for (const auto u : selected) {
      const auto uView = foreignToView[u];
      for (const auto &dimLink : foreignFold->dimensionsOf(u)) {
        const auto dim = dimLink.dim;
        const auto v   = dimLink.neighbor(zigzag::DimVector::POS);
        if (v != zigzag::noCell && foreignToView.contains(v)) {
          const auto vView = foreignToView[v];
          // Ensure or resolve dimension in arena
          const auto dimName  = foreignFold->textOf(dim, *foreignDoc.store);
          const auto arenaDim = arena.ensureDimension(dimName);
          (void)arena.link(uView, arenaDim, zigzag::DimVector::POS, vView);
        }
      }
    }

    // Splice view occurrences onto local ranks containing Q
    for (const auto &link : base.dimensionsOf(qCell)) {
      const auto d = link.dim;
      if (d == *dimQuotes ||
          (base.dimensionNamed(kDimQuotesState, localStore) &&
           d == *base.dimensionNamed(kDimQuotesState, localStore)) ||
          (base.dimensionNamed(kDimQuotesSel, localStore) &&
           d == *base.dimensionNamed(kDimQuotesSel, localStore)) ||
          (base.dimensionNamed(kDimOverrides, localStore) &&
           d == *base.dimensionNamed(kDimOverrides, localStore))) {
        continue;
      }

      // Found local rank dimension on Q
      const auto localSucc = base.linked(qCell, d, zigzag::DimVector::POS);
      const auto entryCell = viewRun.front();
      const auto lastCell  = viewRun.back();

      (void)arena.link(qCell, d, zigzag::DimVector::POS, entryCell);
      if (localSucc != zigzag::noCell) {
        (void)arena.link(lastCell, d, zigzag::DimVector::POS, localSucc);
      }
    }
  }
}

} // namespace xanadu

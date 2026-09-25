/**
 * @file quoted_structure.hpp
 * @brief Quoted structure: adopting somebody else's shape (§5.10).
 *
 * Implements deterministic selectors (cell, rank, closure, query),
 * occurrence-scoped ArenaManifold materialization, override substitution
 * keyed by GlobalOpRef, and foreign document resolution above the fold.
 */
#ifndef COMMON_XANADU_QUOTED_STRUCTURE_HPP
#define COMMON_XANADU_QUOTED_STRUCTURE_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gleditor/cpp26.hpp>
#include <gleditor/ranges.hpp>

#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/zzcore.hpp"

namespace xanadu {

class Store;

// Canonical dimension names for quoted structure (§5.10 §3, §4)
inline constexpr std::string_view kDimQuotes      = "d.quotes";
inline constexpr std::string_view kDimQuotesState = "d.quotes-state";
inline constexpr std::string_view kDimQuotesSel   = "d.quotes-sel";
inline constexpr std::string_view kDimQuotesCarry = "d.quotes-carry";
inline constexpr std::string_view kDimOverrides   = "d.overrides";
inline constexpr std::string_view kDimShadows     = "d.shadows";

// Supported VQL language version for Query selectors (§5.10 §2)
inline constexpr std::uint32_t kSupportedVqlLanguageVersion = 13;

/**
 * @brief Foreign document reference for resolution.
 */
struct ForeignDocument {
  const Store *store{nullptr};
  const Scroll *sealedAs{nullptr};
};

/**
 * @brief Cancellation token for asynchronous foreign requests.
 */
struct CancellationToken {
  std::shared_ptr<std::atomic<bool>> cancelled{
      std::make_shared<std::atomic<bool>>(false)};

  void cancel() noexcept {
    if (cancelled) {
      cancelled->store(true, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] bool isCancelled() const noexcept {
    return cancelled && cancelled->load(std::memory_order_relaxed);
  }
};

/**
 * @brief Request descriptor for fetching foreign document states.
 */
struct ForeignRequest {
  GlobalDocumentState state;
  CancellationToken cancellation;
};

/**
 * @brief Fetch states for foreign document sources.
 */
enum class FetchState : std::uint8_t {
  Ready,
  NotFetched,
  Absent,
  Unintelligible,
  TooLarge
};

/**
 * @brief Quotation resolution lifecycle state.
 */
enum class QuotationState : std::uint8_t {
  Resolved,      ///< The cells are materialized in the arena
  NotFetched,    ///< The bytes are pending or awaiting arrival
  Absent,        ///< Verified permanent absence
  TooLarge,      ///< Answer or foreign operation path exceeded budget
  Unintelligible ///< Unsupported selector or unknown language version
};

/**
 * @brief Budget constraints bounding foreign fetch, fold, and evaluation.
 */
struct QuotationBudget {
  std::uint32_t maxCells{10000};
  std::uint32_t maxDepth{100};
  std::uint64_t maxOpBytes{10 * 1024 * 1024};
  std::uint64_t maxDescriptorBytes{1024 * 1024};
  std::uint64_t maxSessionBytes{50 * 1024 * 1024};
  std::uint64_t maxMaterializedProxies{10000};
  std::chrono::milliseconds deadline{5000};
};

/**
 * @brief Abstract interface for foreign document resolution.
 */
class IForeignSource {
public:
  virtual ~IForeignSource() = default;

  virtual void requestDocument(
      const ForeignRequest &req,
      gleditor::cpp26::function_ref<void(FetchState, ForeignDocument)>
          callback) = 0;
};

/**
 * @brief In-memory foreign document source for testing and local resolution.
 */
class InMemoryForeignSource : public IForeignSource {
public:
  void registerDocument(const GlobalDocumentState &state, const Store *store,
                        const Scroll *sealedAs,
                        FetchState fetchState = FetchState::Ready);

  void setFetchState(const GlobalDocumentState &state, FetchState fetchState);

  void requestDocument(
      const ForeignRequest &req,
      gleditor::cpp26::function_ref<void(FetchState, ForeignDocument)> callback)
      override;

private:
  struct Entry {
    const Store *store{nullptr};
    const Scroll *sealedAs{nullptr};
    FetchState state{FetchState::Ready};
  };

  std::unordered_map<std::string, Entry> entries_;
};

/**
 * @brief Evaluated selector representation operating over foreign refs.
 */
struct Selector {
  enum class Kind : std::uint8_t { Cell, Rank, Closure, Query };

  Kind kind{Kind::Cell};
  zigzag::CellRef root{zigzag::noCell};                    ///< Foreign ref
  zigzag::DimRef rankDim{zigzag::noCell};                  ///< Rank only
  zigzag::DimVector rankDirection{zigzag::DimVector::POS}; ///< Rank only
  std::vector<zigzag::DimRef> carry;                       ///< Closure only
  std::string query;                                       ///< Query only
  std::uint32_t queryLanguageVersion{kSupportedVqlLanguageVersion};
  std::string orderPolicy{"identity"};
};

/**
 * @brief Authoring specification for a selector before foreign resolution.
 */
struct SelectorSpec {
  Selector::Kind kind{Selector::Kind::Cell};
  ExternOpRef rootRef;
  std::optional<ExternOpRef> rankDimRef;
  zigzag::DimVector rankDirection{zigzag::DimVector::POS};
  std::vector<ExternOpRef> carryRefs;
  std::string query;
  std::uint32_t queryLanguageVersion{kSupportedVqlLanguageVersion};
  std::string orderPolicy{"identity"};

  bool operator==(const SelectorSpec &) const = default;
};

/**
 * @brief Return value for Store::quote().
 */
struct AppendedQuotation {
  MicroversionId version;
  zigzag::CellRef quotationCell{zigzag::noCell};   ///< Q on local rank
  zigzag::CellRef placeholderCell{zigzag::noCell}; ///< Root placeholder
  zigzag::CellRef stateCell{zigzag::noCell};       ///< Pinned state descriptor
  zigzag::CellRef selectorCell{zigzag::noCell};    ///< Selector descriptor
};

/**
 * @brief Serializes a selector specification into descriptor cell text.
 */
[[nodiscard]] std::string writeSelectorDescriptor(const SelectorSpec &spec);

/**
 * @brief Parses selector specification metadata from descriptor cell text.
 */
[[nodiscard]] std::optional<SelectorSpec>
readSelectorDescriptor(std::string_view text);

/**
 * @brief Local override entry shadowing a foreign cell.
 */
struct OverrideEntry {
  zigzag::CellRef overrideCell{zigzag::noCell};    ///< Local replacement cell
  zigzag::CellRef placeholderCell{zigzag::noCell}; ///< P_target
  GlobalOpRef targetGlobalRef;                     ///< Keyed by GlobalOpRef
};

/**
 * @brief Authored quotation parsed from local store & manifold.
 */
struct Quotation {
  zigzag::CellRef head{zigzag::noCell};            ///< Q on local rank
  zigzag::CellRef placeholderCell{zigzag::noCell}; ///< Root placeholder
  zigzag::CellRef stateCell{zigzag::noCell};       ///< Pinned state descriptor
  zigzag::CellRef selectorCell{zigzag::noCell};    ///< Selector descriptor
  GlobalDocumentState pinnedState;
  SelectorSpec selectorSpec;
  std::vector<OverrideEntry> overrides;
  std::string label;
};

/**
 * @brief Resolution result for a single quotation.
 */
struct ResolvedQuotation {
  zigzag::CellRef head{zigzag::noCell};
  QuotationState state{QuotationState::NotFetched};
  std::uint32_t cellCount{0};
  std::vector<zigzag::CellRef> materializedOccurrences;
};

/**
 * @brief Reads an authored quotation at cell @p qCell.
 */
[[nodiscard]] std::optional<Quotation>
readQuotation(const zigzag::Manifold &manifold, const Store &store,
              zigzag::CellRef qCell);

/**
 * @brief Evaluates a selector against a folded foreign store and manifold.
 */
[[nodiscard]] std::vector<zigzag::CellRef>
evaluateSelector(const Selector &selector,
                 const zigzag::Manifold &foreignManifold,
                 const Store &foreignStore, const QuotationBudget &budget,
                 QuotationState &outState);

/**
 * @brief Resolves all quotations in @p base into @p arena.
 *
 * Traverses reachable quotation heads, checks visited guards to break cycles,
 * attaches foreign spaces, evaluates selectors, substitutes overrides by
 * GlobalOpRef, links only induced edges, and splices occurrences into the
 * local rank while preserving successors. Navigation writes nothing to local
 * store (R8).
 */
void resolveQuotations(zigzag::ArenaManifold &arena,
                       const zigzag::Manifold &base, const Store &localStore,
                       IForeignSource &source, QuotationBudget budget,
                       std::uint32_t viewEpoch = 0);

} // namespace xanadu

#endif // COMMON_XANADU_QUOTED_STRUCTURE_HPP

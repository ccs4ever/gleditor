/**
 * @file quotation_builder.hpp
 * @brief Interactive quotation builder model and preview engine (§5.10).
 *
 * Provides structured configuration for foreign store selection, root cell
 * picking, selector definition (Rank, Closure, VQL Query), navigable preview
 * generation, and quotation commitment into a persistent local store.
 */
#ifndef COMMON_XANADU_QUOTATION_BUILDER_HPP
#define COMMON_XANADU_QUOTATION_BUILDER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/quoted_structure.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/zzcore.hpp"

namespace xanadu {

/**
 * @brief Configuration data for building a quotation.
 */
struct QuotationBuilderConfig {
  std::string foreignScrollKey;
  MicroversionId pinnedVersion;
  ExternOpRef rootRef;
  std::string rootLabel;
  Selector::Kind mode{Selector::Kind::Closure};

  // Rank mode configuration
  std::optional<ExternOpRef> rankDimRef;
  zigzag::DimVector rankDirection{zigzag::DimVector::POS};

  // Closure mode configuration
  std::vector<ExternOpRef> carryRefs;

  // Custom VQL Query configuration
  std::string vqlQuery;
  std::uint32_t queryLanguageVersion{kSupportedVqlLanguageVersion};

  // Local document placement
  std::string localRankDimName{"d.vars"};
  std::string quotationLabel{"Quotation"};
};

/**
 * @brief A single previewed cell within the quotation answer set.
 */
struct PreviewCell {
  zigzag::CellRef foreignCell{zigzag::noCell};
  std::string text;
  std::string role;
  ExternOpRef birthRef;
  std::vector<std::pair<std::string, zigzag::CellRef>> outboundEdges;
};

/**
 * @brief Live preview state of the quotation being constructed.
 */
struct QuotationPreviewState {
  bool isValid{false};
  QuotationState state{QuotationState::NotFetched};
  std::string statusMessage;
  std::vector<PreviewCell> cells;
  std::size_t focusedIndex{0};
  std::uint64_t totalOpBytes{0};
  std::uint32_t totalProxies{0};
};

/**
 * @class QuotationBuilder
 * @brief Builder engine for configuring, previewing, and committing quotations.
 */
class QuotationBuilder {
public:
  QuotationBuilder() = default;

  void
  setForeignStore(std::string scrollKey, const Store *store,
                  const Scroll *sealedAs                      = nullptr,
                  std::optional<MicroversionId> pinnedVersion = std::nullopt);

  void setRootCell(zigzag::CellRef cell);
  void setRootRef(ExternOpRef ref, std::string label = {});

  void setMode(const Selector::Kind mode) noexcept { config_.mode = mode; }
  [[nodiscard]] Selector::Kind mode() const noexcept { return config_.mode; }

  // Rank mode settings
  void setRankStep(ExternOpRef dimRef,
                   zigzag::DimVector dir = zigzag::DimVector::POS);

  // Closure mode settings
  void setCarriedDimensions(std::vector<ExternOpRef> carryRefs);
  void addCarriedDimension(ExternOpRef dimRef);
  void removeCarriedDimension(const ExternOpRef &dimRef);

  // Query mode settings
  void setVqlQuery(std::string query,
                   std::uint32_t version = kSupportedVqlLanguageVersion);

  // Local placement and label
  void setLocalRankDim(std::string dimName) {
    config_.localRankDimName = std::move(dimName);
  }
  void setQuotationLabel(std::string label) {
    config_.quotationLabel = std::move(label);
  }

  [[nodiscard]] const QuotationBuilderConfig &config() const noexcept {
    return config_;
  }
  [[nodiscard]] QuotationBuilderConfig &config() noexcept { return config_; }

  [[nodiscard]] const QuotationPreviewState &preview() const noexcept {
    return preview_;
  }

  void recomputePreview(const QuotationBudget &budget = QuotationBudget{
                            .maxCells   = 500,
                            .maxDepth   = 50,
                            .maxOpBytes = 10 * 1024 * 1024,
                        });

  void focusPreviewCell(std::size_t index) noexcept;
  void navigatePreview(int delta) noexcept;

  [[nodiscard]] AppendedQuotation commit(Store &localStore,
                                         const MicroversionId &parent,
                                         zigzag::CellRef localTail,
                                         zigzag::DimRef localDim);

private:
  QuotationBuilderConfig config_;
  QuotationPreviewState preview_;

  const Store *foreignStore_{nullptr};
  const Scroll *sealedAs_{nullptr};
  std::shared_ptr<zigzag::Manifold> foreignFold_;
};

} // namespace xanadu

#endif // COMMON_XANADU_QUOTATION_BUILDER_HPP

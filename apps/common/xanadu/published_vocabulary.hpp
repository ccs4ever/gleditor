/**
 * @file published_vocabulary.hpp
 * @brief Published vocabularies: dimension identity across documents (§5.11).
 *
 * Implements Section 5.11 of structure-hyperop-vision.md:
 * - PublishedVocabulary DTO: releaseCell, state, dimensions.
 * - Release publication: touches releaseCell last to pin foreign state.
 * - Vocabulary adoption: §5.10 rank quotation over d.dims.
 * - Term filing: interns §5.5 placeholder and links to local d.dims.
 */
#ifndef COMMON_XANADU_PUBLISHED_VOCABULARY_HPP
#define COMMON_XANADU_PUBLISHED_VOCABULARY_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/quoted_structure.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu {

/**
 * @brief Publication DTO for a vocabulary release (§5.11 §3).
 */
struct PublishedVocabulary {
  GlobalOpRef releaseCell;   ///< birth op: stable identity of the release cell
  GlobalDocumentState state; ///< pinned publication snapshot
  GlobalOpRef dimensions;    ///< birth op of the d.dims cell
};

struct VocabularyReleaseResult {
  MicroversionId version;
  PublishedVocabulary vocabulary;
  zigzag::CellRef releaseCell{zigzag::noCell};
};

/**
 * @brief Publish a vocabulary release by touching releaseCell last (§5.11 §3).
 *
 * Ensures terms are linked along d.dims starting posward from releaseCell,
 * then restates releaseCell's content/value with releaseLabel to pin the
 * foreign state.
 */
[[nodiscard]] VocabularyReleaseResult publishVocabularyRelease(
    Store &store, const MicroversionId &parent, zigzag::CellRef releaseCell,
    std::string_view releaseLabel, std::span<const zigzag::CellRef> terms);

/**
 * @brief Adopt a published vocabulary as a §5.10 rank quotation (§5.11 §1, §7).
 *
 * Quoting a vocabulary makes its catalogue visible and navigable as ephemeral
 * occurrences, without filing every term into the local store.
 */
[[nodiscard]] AppendedQuotation
adoptVocabulary(Store &localStore, const MicroversionId &parent,
                const PublishedVocabulary &vocab, std::string_view localLabel,
                zigzag::CellRef localRankTail = zigzag::noCell,
                zigzag::DimRef localRankDim   = zigzag::noCell);

/**
 * @brief Result of filing a term for local use (§5.11 §4).
 */
struct AdoptedDimension {
  MicroversionId version;
  zigzag::DimRef local{zigzag::noCell};
};

/**
 * @brief File a published term as a persistent local dimension (§5.11 §4).
 *
 * 1. Localises term via §5.5 and mints or reuses its persistent placeholder on
 *    d.scroll-refs.
 * 2. Links the placeholder to local d.dims, making it a valid DimRef.
 * 3. Interns by GlobalOpRef: filing the same term twice reuses the existing
 *    placeholder.
 * 4. If localAlias is non-empty, attaches an alias cell along d.alias and
 *    registers with DimensionRegistry.
 */
[[nodiscard]] AdoptedDimension
adoptPublishedDimension(Store &store, const MicroversionId &parent,
                        const GlobalOpRef &term,
                        std::string_view localAlias = {});

/**
 * @brief Check if @p term is already filed on local d.dims in @p store.
 */
[[nodiscard]] std::optional<zigzag::DimRef>
findAdoptedDimension(const Store &store, const GlobalOpRef &term,
                     const MicroversionId &version = {});

/**
 * @brief Authored repoint of an adopted vocabulary quotation to a new release
 * (§5.11 §3, §8).
 *
 * Mints new state and selector descriptors for @p newVocab and repoints
 * @p quotationCell to them, recording the update in the quotation head's R7
 * chain.
 */
[[nodiscard]] MicroversionId
updateAdoptedVocabulary(Store &localStore, const MicroversionId &parent,
                        zigzag::CellRef quotationCell,
                        const PublishedVocabulary &newVocab);

} // namespace xanadu

#endif // COMMON_XANADU_PUBLISHED_VOCABULARY_HPP

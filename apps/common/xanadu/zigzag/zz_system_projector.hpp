/**
 * @file zz_system_projector.hpp
 * @brief Bidirectional projection between Project Xanadu Zigzag
 * multidimensional configuration slices and sovereign 3-page system xanadocs.
 */
#ifndef XANADU_ZIGZAG_SYSTEM_PROJECTOR_HPP
#define XANADU_ZIGZAG_SYSTEM_PROJECTOR_HPP

#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"

namespace xanadu {
class Store;
} // namespace xanadu

namespace zigzag {

/// Standard dimensional axes for system configuration slices.
inline constexpr std::string_view kDimConfig = "d.config";
inline constexpr std::string_view kDimValue  = "d.value";
inline constexpr std::string_view kDimSchema = "d.schema";
inline constexpr std::string_view kDimNotes  = "d.notes";
inline constexpr std::string_view kDimGroup  = "d.group";

/**
 * @brief Linearize active runtime configuration key-value lines from a slice.
 *
 * Walks the d.config rank in topological order, extracting all cells of type
 * "setting" (or formatted as key: value).
 */
[[nodiscard]] std::string
extractSliceConfigText(const ZzStructureDocument &slice);

/**
 * @brief Linearize schema and purpose documentation from a slice.
 *
 * Traverses d.schema rank, beginning with the schema_doc cell followed by
 * individual schema_field annotations.
 */
[[nodiscard]] std::string
extractSliceSchemaText(const ZzStructureDocument &slice);

/**
 * @brief Linearize user annotations and calibration notes from a slice.
 *
 * Traverses d.notes rank, starting from the user_notes header cell followed
 * by field notes.
 */
[[nodiscard]] std::string
extractSliceNotesText(const ZzStructureDocument &slice);

/**
 * @brief Projects a Zigzag configuration slice into a sovereign 3-page Store.
 *
 * Strictly adheres to Project Xanadu system document governance:
 * - Page 1: Linearized active configuration text.
 * - Page 2: Schema & Purpose with centered, bold headers via format links (zero
 * markdown).
 * - Page 3: Notes with centered, bold headers via format links (zero markdown).
 * - Page breaks separating pages.
 * - Butterfly comment links interconnecting Page 1 setting spans to Page 2
 * schema spans and Page 3 notes spans.
 *
 * @param slice The input Zigzag slice document.
 * @param store The destination sovereign Store.
 * @param kind The system doc kind (e.g. SystemDocKind::Layout, Settings, etc.).
 * @return The final MicroversionId of the created document.
 */
xanadu::MicroversionId
projectSystemSliceToStore(const ZzStructureDocument &slice,
                          xanadu::Store &store, xanadu::SystemDocKind kind);

/**
 * @brief Reverse projection: constructs a multidimensional Zigzag slice from a
 *        3-page sovereign Store.
 *
 * @param store The source sovereign Store.
 * @param kind The system doc kind.
 * @return A ZzStructureDocument representing the multidimensional configuration
 * space.
 */
[[nodiscard]] ZzStructureDocument
projectSystemStoreToSlice(const xanadu::Store &store,
                          xanadu::SystemDocKind kind);

} // namespace zigzag

#endif // XANADU_ZIGZAG_SYSTEM_PROJECTOR_HPP

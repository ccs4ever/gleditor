/**
 * @file extern_ref.hpp
 * @brief Persistent references to cells in other stores (§5.5).
 */
#ifndef XUDU_EXTERN_REF_HPP
#define XUDU_EXTERN_REF_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "microversion.hpp"
#include "spool.hpp"
#include "zigzag/dim_vector.hpp"
#include <gleditor/cpp26.hpp>

namespace zigzag {
class Manifold;
} // namespace zigzag

namespace xanadu {

class Store;
struct Scroll;

/**
 * @brief Reference to an operation through a store-local scroll ID.
 *
 * Names one operation through a store-local scroll id. When naming a cell,
 * `produces` must be its MakeCell birth operation.
 */
struct ExternOpRef {
  ScrollId scroll{localScroll};
  MicroversionId produces;

  bool operator==(const ExternOpRef &) const  = default;
  auto operator<=>(const ExternOpRef &) const = default;
};

/**
 * @brief Snapshot descriptor for pouch provenance and overlay targets.
 *
 * Names a whole document snapshot through its global scroll key, without
 * pretending that some arbitrarily last operation is the document.
 */
struct GlobalDocumentState {
  std::string scroll;
  MicroversionId version;

  bool operator==(const GlobalDocumentState &) const  = default;
  auto operator<=>(const GlobalDocumentState &) const = default;
};

/**
 * @brief Versioned, length-prefixed serialization for GlobalDocumentState.
 *
 * Codec for descriptor content stored in cells.
 */
[[nodiscard]] std::string
writeGlobalDocumentState(const GlobalDocumentState &state);

[[nodiscard]] std::optional<GlobalDocumentState>
readGlobalDocumentState(std::string_view bytes);

/**
 * @brief Status of resolving an external cell placeholder against a foreign
 * store.
 */
enum class ExternResolutionStatus : std::uint8_t {
  Resolved,   ///< Placeholder resolved to a foreign MakeCell.
  NotFetched, ///< Foreign document or descriptor content unavailable.
  Absent,     ///< Foreign history definitely lacks the requested microversion.
  Unintelligible, ///< Target resolves to a non-MakeCell (SetValue/SetLink) or
                  ///< malformed.
};

[[nodiscard]] constexpr std::string_view
toString(const ExternResolutionStatus status) noexcept {
  switch (status) {
  case ExternResolutionStatus::Resolved:
    return "resolved";
  case ExternResolutionStatus::NotFetched:
    return "not fetched";
  case ExternResolutionStatus::Absent:
    return "absent";
  case ExternResolutionStatus::Unintelligible:
    return "unintelligible";
  }
  return "unknown";
}

struct ExternResolution {
  ExternResolutionStatus status{ExternResolutionStatus::NotFetched};
  zigzag::CellRef cell{zigzag::noCell};
  std::uint32_t opIndex{0};

  [[nodiscard]] bool isResolved() const noexcept {
    return status == ExternResolutionStatus::Resolved;
  }

  bool operator==(const ExternResolution &) const = default;
};

/**
 * @brief Synchronous resolution of an extern ref placeholder against an opened
 * foreign store.
 */
[[nodiscard]] ExternResolution
resolveExternCell(const Store &localStore, zigzag::CellRef placeholder,
                  const Store &foreignStore, const Scroll &sealedAs,
                  const zigzag::Manifold *foreignFold = nullptr);

} // namespace xanadu

#endif // XUDU_EXTERN_REF_HPP

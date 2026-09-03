/**
 * @file source_grounder.hpp
 * @brief Locating and verifying subspan offsets within parent source documents
 * and media.
 */
#ifndef GLEDITOR_SOURCE_GROUNDER_HPP
#define GLEDITOR_SOURCE_GROUNDER_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gleditor {

/**
 * @brief Result of locating an excerpt or subclip within a parent source work.
 */
struct SubspanMatch {
  std::uint64_t offset{};
  std::uint64_t length{};
  std::string mimeType;
  std::string sourcePathOrMagnet;
  std::string contextSnippet;
};

/**
 * @brief A known or cached source document/media available for quick selection.
 */
struct KnownSource {
  std::string displayName;
  std::string pathOrMagnet;
  std::string mimeType;
  std::string infoHash;
};

/**
 * @class SourceGrounder
 * @brief Algorithms for subspan matching across UTF-8 text and binary payloads.
 */
class SourceGrounder {
public:
  /**
   * @brief Search for @p quoteSnippet within @p parentText.
   *
   * Tries exact substring match first. If not found, attempts whitespace-
   * normalized matching (folding consecutive spaces and newlines) while
   * returning exact byte offsets into the original @p parentText.
   */
  [[nodiscard]] static std::optional<SubspanMatch>
  locateTextSubspan(std::string_view parentText, std::string_view quoteSnippet);

  /**
   * @brief Search for a contiguous @p excerptBytes sub-buffer inside @p
   * parentBytes.
   */
  [[nodiscard]] static std::optional<SubspanMatch>
  locateBinarySubspan(std::span<const std::uint8_t> parentBytes,
                      std::span<const std::uint8_t> excerptBytes);

  /**
   * @brief Read a local file and attempt to locate @p excerptSnippet within it.
   */
  [[nodiscard]] static std::optional<SubspanMatch>
  groundFile(const std::filesystem::path &sourceFile,
             std::string_view excerptSnippet);

  /**
   * @brief Read a local file and attempt to locate @p excerptBytes within it.
   */
  [[nodiscard]] static std::optional<SubspanMatch>
  groundFileBinary(const std::filesystem::path &sourceFile,
                   std::span<const std::uint8_t> excerptBytes);

  /**
   * @brief Compute the lowercase hexadecimal SHA-256 digest of bytes.
   */
  [[nodiscard]] static std::string
  computeSha256(std::span<const std::uint8_t> bytes);

  [[nodiscard]] static std::string computeSha256(std::string_view text);
};

} // namespace gleditor

#endif // GLEDITOR_SOURCE_GROUNDER_HPP

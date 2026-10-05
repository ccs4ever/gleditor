/**
 * @file document_id.hpp
 * @brief A stable identity for one local xanadoc.
 */
#ifndef XANADU_DOCUMENT_ID_HPP
#define XANADU_DOCUMENT_ID_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace xanadu {

/**
 * @brief A random, persisted name for a Store, distinct from its revisions.
 *
 * A MicroversionId says which state an edit history has reached. It cannot
 * identify the document containing that history: two stores may both have a
 * state named "1". This value is written in store.tables and is the durable
 * document half of a semantic pick target.
 */
class DocumentId {
public:
  DocumentId() { generate(); }

  [[nodiscard]] const std::array<std::uint8_t, 16> &bytes() const noexcept {
    return bytes_;
  }

  [[nodiscard]] std::string str() const {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out{kPrefix};
    out.reserve(out.size() + bytes_.size() * 2U);
    for (const auto byte : bytes_) {
      out.push_back(hex[byte >> 4U]);
      out.push_back(hex[byte & 0x0fU]);
    }
    return out;
  }

  [[nodiscard]] static bool fromBytes(const std::string_view value,
                                      DocumentId &out) {
    if (value.size() != out.bytes_.size()) {
      return false;
    }
    std::copy_n(reinterpret_cast<const std::uint8_t *>(value.data()),
                out.bytes_.size(), out.bytes_.begin());
    return true;
  }

  /// The id @ref str wrote, with or without its "local:" prefix.
  [[nodiscard]] static std::optional<DocumentId> parse(std::string_view text) {
    text.remove_prefix(text.starts_with(kPrefix) ? kPrefix.size() : 0);
    DocumentId out;
    if (text.size() != out.bytes_.size() * 2U) {
      return std::nullopt;
    }
    const auto nibble = [](const char digit) -> int {
      if (digit >= '0' && digit <= '9') return digit - '0';
      if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
      return -1;
    };
    for (std::size_t i = 0; i < out.bytes_.size(); ++i) {
      const auto high = nibble(text[2 * i]);
      const auto low  = nibble(text[(2 * i) + 1]);
      if (high < 0 || low < 0) {
        return std::nullopt;
      }
      out.bytes_[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return out;
  }

  bool operator==(const DocumentId &) const = default;

private:
  static constexpr std::string_view kPrefix = "local:";

  void generate() {
    std::random_device random;
    for (auto &byte : bytes_) {
      byte = static_cast<std::uint8_t>(random());
    }
    bytes_[0] |= 1U;
  }

  std::array<std::uint8_t, 16> bytes_{};
};

} // namespace xanadu

#endif // XANADU_DOCUMENT_ID_HPP

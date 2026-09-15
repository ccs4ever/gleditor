/**
 * @file document_id.hpp
 * @brief A stable identity for one local xanadoc.
 */
#ifndef XANADU_DOCUMENT_ID_HPP
#define XANADU_DOCUMENT_ID_HPP

#include <algorithm>
#include <array>
#include <cstdint>
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
    std::string out{"local:"};
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

  bool operator==(const DocumentId &) const = default;

private:
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

/**
 * @file spool.hpp
 * @brief Addresses for content, and the local spool that is one home for it.
 */
#ifndef XUDU_SPOOL_H
#define XUDU_SPOOL_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace xudu {

using ScrollId                        = std::uint32_t;
inline constexpr ScrollId localScroll = 0;

inline constexpr ScrollId breakMarkerScroll =
    std::numeric_limits<ScrollId>::max();

inline constexpr ScrollId vocabularyScroll = breakMarkerScroll - 1;

/**
 * @brief A run of content at a permanent address.
 */
struct PrimediaSpan {
  ScrollId scroll{localScroll};
  std::uint64_t start{};
  std::uint64_t length{};

  [[nodiscard]] std::uint64_t end() const { return start + length; }
  [[nodiscard]] bool empty() const { return 0 == length; }
  [[nodiscard]] bool isLocal() const { return localScroll == scroll; }
  [[nodiscard]] bool contains(const ScrollId which,
                              const std::uint64_t address) const {
    return which == scroll && address >= start && address < end();
  }

  [[nodiscard]] PrimediaSpan intersect(const PrimediaSpan &other) const {
    if (scroll != other.scroll) {
      return {scroll, start, 0};
    }
    const auto from = std::max(start, other.start);
    const auto to   = std::min(end(), other.end());
    return to > from ? PrimediaSpan{scroll, from, to - from}
                     : PrimediaSpan{scroll, from, 0};
  }

  [[nodiscard]] PrimediaSpan slice(const std::uint64_t offset,
                                   const std::uint64_t count) const {
    const auto from = std::min(offset, length);
    return {scroll, start + from, std::min(count, length - from)};
  }

  bool operator==(const PrimediaSpan &) const = default;
};

/**
 * @brief Something that can turn an address back into bytes.
 */
class SpanReader {
public:
  SpanReader()          = default;
  virtual ~SpanReader() = default;

  SpanReader(const SpanReader &)            = delete;
  SpanReader &operator=(const SpanReader &) = delete;
  SpanReader(SpanReader &&)                 = delete;
  SpanReader &operator=(SpanReader &&)      = delete;

  [[nodiscard]] virtual std::string read(const PrimediaSpan &span) const = 0;
};

/**
 * @class PrimediaSpool
 * @brief Append-and-read-only storage for content typed here.
 */
class PrimediaSpool : public SpanReader {
public:
  PrimediaSpool();
  ~PrimediaSpool() override;

  PrimediaSpan append(std::string_view bytes);

  [[nodiscard]] std::string read(const PrimediaSpan &span) const override;

  [[nodiscard]] std::string_view readView(const PrimediaSpan &span) const;

  [[nodiscard]] std::uint64_t size() const;

  [[nodiscard]] std::string_view bytes() const;

  void adopt(std::string_view stored);

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace xudu

#endif // XUDU_SPOOL_H

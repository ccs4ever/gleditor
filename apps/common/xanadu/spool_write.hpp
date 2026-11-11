/**
 * @file spool_write.hpp
 * @brief Named refusal for mutation attempts on read-only spools.
 */
#ifndef XANADU_SPOOL_WRITE_HPP
#define XANADU_SPOOL_WRITE_HPP

#include <expected>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace xanadu {

enum class SpoolRefusal { ReadOnly };

template <class T>
using SpoolResult = std::expected<T, SpoolRefusal>;

class SpoolWriteRefused : public std::runtime_error {
public:
  explicit SpoolWriteRefused(SpoolRefusal refusal,
                             std::string_view action = "write spool")
      : std::runtime_error(message(refusal, action)) {}

private:
  static std::string message(SpoolRefusal refusal, std::string_view action) {
    switch (refusal) {
    case SpoolRefusal::ReadOnly:
      return std::string(action) + " refused: spool is read-only";
    }
    return std::string(action) + " refused: unknown spool refusal";
  }
};

template <class T>
T requireSpoolWrite(SpoolResult<T> result,
                    std::string_view action = "write spool") {
  if (!result) {
    throw SpoolWriteRefused(result.error(), action);
  }
  if constexpr (std::is_void_v<T>) {
    return;
  } else {
    return std::move(*result);
  }
}

} // namespace xanadu

#endif // XANADU_SPOOL_WRITE_HPP

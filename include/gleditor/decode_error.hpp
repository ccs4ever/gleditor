/**
 * @file decode_error.hpp
 * @brief Why a media decoder produced nothing.
 *
 * Shared by every decoder in the library -- still images, GIF, animated SVG
 * -- so a caller can tell "this build has no decoder for it" from "the bytes
 * are wrong", which the old nullptr and invalid-image returns could not.
 */
#ifndef GLEDITOR_DECODE_ERROR_HPP
#define GLEDITOR_DECODE_ERROR_HPP

#include <cstdint>
#include <string_view>

namespace gleditor {

enum class DecodeError : std::uint8_t {
  Empty,         ///< no bytes, or no file to read them from
  Truncated,     ///< fewer bytes than the format's own header needs
  Undecodable,   ///< the decoder rejected them: corrupt, or unsupported
  Unconvertible, ///< decoded, but could not be converted to RGBA8
  NotAnimated,   ///< a valid document with nothing to animate
  NoCodec,       ///< this build was compiled without the decoder
};

[[nodiscard]] constexpr std::string_view
toString(const DecodeError error) noexcept {
  switch (error) {
  case DecodeError::Empty:
    return "empty";
  case DecodeError::Truncated:
    return "truncated";
  case DecodeError::Undecodable:
    return "undecodable";
  case DecodeError::Unconvertible:
    return "not convertible to RGBA8";
  case DecodeError::NotAnimated:
    return "nothing to animate";
  case DecodeError::NoCodec:
    return "decoder not built in";
  }
  return "undecodable";
}

} // namespace gleditor

#endif // GLEDITOR_DECODE_ERROR_HPP

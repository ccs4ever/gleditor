#ifndef COMMON_XANADU_VORTEX_STDLIB_IMAGE_FORMAT_HPP
#define COMMON_XANADU_VORTEX_STDLIB_IMAGE_FORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace zigzag::vortex {

inline constexpr std::int64_t kStdlibContentVersion = 1;

/// Canonical, human-readable ABI input to the image epoch digest. Includes
/// decode boundaries because identical opcode labels can still decode
/// differently.
[[nodiscard]] std::string stdlibImageAbiText();
[[nodiscard]] std::string stdlibImageEpoch();
[[nodiscard]] std::size_t stdlibImageOpcodeCount() noexcept;
[[nodiscard]] std::size_t stdlibImageVerbCount() noexcept;

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_STDLIB_IMAGE_FORMAT_HPP

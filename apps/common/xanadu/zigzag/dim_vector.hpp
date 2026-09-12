/**
 * @file dim_vector.hpp
 * @brief Directional vector and directed dimension primitives for ZigZag space.
 */
#ifndef COMMON_XANADU_ZIGZAG_DIM_VECTOR_HPP
#define COMMON_XANADU_ZIGZAG_DIM_VECTOR_HPP

#include <compare>
#include <cstdint>
#include <format>
#include <string_view>
#include <type_traits>

namespace zigzag {

using CellRef                   = std::uint32_t;
using DimRef                    = CellRef;
inline constexpr CellRef noCell = 0;

/**
 * @enum DimVector
 * @brief Discrete unit directional vector along a 1D ZigZag dimensional axis.
 *
 * Underlying type is std::int8_t.
 * POS corresponds to posward (+1), NEG corresponds to negward (-1).
 */
enum class DimVector : std::int8_t {
  NEG = -1,
  POS = 1,
};

inline constexpr DimVector Negward = DimVector::NEG;
inline constexpr DimVector Posward = DimVector::POS;

// -- DimVector Inversion & Query Helpers -------------------------------------

[[nodiscard]] constexpr DimVector operator-(const DimVector v) noexcept {
  return (v == DimVector::POS) ? DimVector::NEG : DimVector::POS;
}

[[nodiscard]] constexpr DimVector operator+(const DimVector v) noexcept {
  return v;
}

[[nodiscard]] constexpr DimVector operator*(const DimVector a,
                                            const DimVector b) noexcept {
  return (a == b) ? DimVector::POS : DimVector::NEG;
}

[[nodiscard]] constexpr bool isPos(const DimVector v) noexcept {
  return v == DimVector::POS;
}

[[nodiscard]] constexpr bool isNeg(const DimVector v) noexcept {
  return v == DimVector::NEG;
}

[[nodiscard]] constexpr bool isPosward(const DimVector v) noexcept {
  return v == DimVector::POS;
}

[[nodiscard]] constexpr bool isNegward(const DimVector v) noexcept {
  return v == DimVector::NEG;
}

[[nodiscard]] constexpr std::int8_t toSign(const DimVector v) noexcept {
  return static_cast<std::int8_t>(v);
}

[[nodiscard]] constexpr DimVector fromSign(const std::int64_t sign) noexcept {
  return sign < 0 ? DimVector::NEG : DimVector::POS;
}

[[nodiscard]] constexpr DimVector fromNegward(const bool negward) noexcept {
  return negward ? DimVector::NEG : DimVector::POS;
}

[[nodiscard]] constexpr bool toNegward(const DimVector v) noexcept {
  return v == DimVector::NEG;
}

[[nodiscard]] constexpr std::string_view toString(const DimVector v) noexcept {
  return (v == DimVector::POS) ? "POS" : "NEG";
}

[[nodiscard]] constexpr std::string_view toSymbol(const DimVector v) noexcept {
  return (v == DimVector::POS) ? "+" : "-";
}

/**
 * @struct DirectedDim
 * @brief Cache-aligned pairing of a dimension reference and a directional
 * vector.
 *
 * Packed into 8 bytes (alignas 4):
 * - dim (4 bytes) at offset 0
 * - dir (1 byte) at offset 4
 * - pad (3 bytes) at offset 5..7
 * Passed in a single 64-bit general-purpose CPU register (%rdx in SysV AMD64
 * ABI).
 */
struct alignas(4) DirectedDim {
  DimRef dim{noCell};
  DimVector dir{DimVector::POS};
  std::uint8_t pad_[3]{0, 0, 0};

  constexpr DirectedDim() noexcept = default;
  constexpr DirectedDim(const DimRef d,
                        const DimVector v = DimVector::POS) noexcept
      : dim(d), dir(v) {}
  constexpr DirectedDim(const DimRef d, const bool negward) noexcept
      : dim(d), dir(fromNegward(negward)) {}

  constexpr bool operator==(const DirectedDim &) const noexcept  = default;
  constexpr auto operator<=>(const DirectedDim &) const noexcept = default;

  [[nodiscard]] constexpr bool isPosward() const noexcept {
    return dir == DimVector::POS;
  }
  [[nodiscard]] constexpr bool isNegward() const noexcept {
    return dir == DimVector::NEG;
  }
  [[nodiscard]] constexpr bool isPos() const noexcept {
    return dir == DimVector::POS;
  }
  [[nodiscard]] constexpr bool isNeg() const noexcept {
    return dir == DimVector::NEG;
  }

  [[nodiscard]] constexpr DirectedDim reversed() const noexcept {
    return DirectedDim{dim, -dir};
  }

  [[nodiscard]] constexpr DirectedDim operator-() const noexcept {
    return reversed();
  }

  [[nodiscard]] constexpr DirectedDim operator+() const noexcept {
    return *this;
  }

  [[nodiscard]] static constexpr DirectedDim pos(const DimRef d) noexcept {
    return DirectedDim{d, DimVector::POS};
  }

  [[nodiscard]] static constexpr DirectedDim neg(const DimRef d) noexcept {
    return DirectedDim{d, DimVector::NEG};
  }
};

static_assert(sizeof(DirectedDim) == 8, "DirectedDim must be exactly 8 bytes");
static_assert(alignof(DirectedDim) == 4, "DirectedDim must align to 4 bytes");
static_assert(std::is_trivially_copyable_v<DirectedDim>,
              "DirectedDim must be trivially copyable");
static_assert(std::is_standard_layout_v<DirectedDim>,
              "DirectedDim must be standard layout");

[[nodiscard]] constexpr DirectedDim operator*(const DimRef dim,
                                              const DimVector dir) noexcept {
  return DirectedDim{dim, dir};
}

[[nodiscard]] constexpr DirectedDim operator*(const DimVector dir,
                                              const DimRef dim) noexcept {
  return DirectedDim{dim, dir};
}

[[nodiscard]] constexpr DirectedDim operator*(const DirectedDim dd,
                                              const DimVector dir) noexcept {
  return DirectedDim{dd.dim, dd.dir * dir};
}

[[nodiscard]] constexpr DirectedDim operator*(const DimVector dir,
                                              const DirectedDim dd) noexcept {
  return DirectedDim{dd.dim, dir * dd.dir};
}

[[nodiscard]] constexpr DirectedDim pos(const DimRef dim) noexcept {
  return DirectedDim::pos(dim);
}

[[nodiscard]] constexpr DirectedDim neg(const DimRef dim) noexcept {
  return DirectedDim::neg(dim);
}

namespace dims {
inline constexpr DimVector POS     = DimVector::POS;
inline constexpr DimVector NEG     = DimVector::NEG;
inline constexpr DimVector Posward = DimVector::POS;
inline constexpr DimVector Negward = DimVector::NEG;
} // namespace dims

} // namespace zigzag

template <>
struct std::formatter<zigzag::DimVector> : std::formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const zigzag::DimVector v, FormatContext &ctx) const {
    return std::formatter<std::string_view>::format(
        v == zigzag::DimVector::POS ? "POS" : "NEG", ctx);
  }
};

template <>
struct std::formatter<zigzag::DirectedDim> : std::formatter<std::string> {
  template <typename FormatContext>
  auto format(const zigzag::DirectedDim &dd, FormatContext &ctx) const {
    return std::formatter<std::string>::format(
        std::format("{}{}", dd.dir == zigzag::DimVector::POS ? "+" : "-",
                    dd.dim),
        ctx);
  }
};

#endif // COMMON_XANADU_ZIGZAG_DIM_VECTOR_HPP

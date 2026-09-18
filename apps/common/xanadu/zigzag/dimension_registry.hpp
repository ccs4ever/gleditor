/**
 * @file dimension_registry.hpp
 * @brief Unified dimension registry with string interning and store-dimension
 *        resolution for ZigZag space.
 */
#ifndef COMMON_XANADU_ZIGZAG_DIMENSION_REGISTRY_HPP
#define COMMON_XANADU_ZIGZAG_DIMENSION_REGISTRY_HPP

#include <compare>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu {
class Store;
class MicroversionId;
} // namespace xanadu

namespace zigzag {

class Manifold;

/**
 * @class InternedDimName
 * @brief Lightweight, interned representation of a ZigZag dimension name.
 *
 * Comparing two interned dimension names is an O(1) integer comparison.
 */
class InternedDimName {
public:
  constexpr InternedDimName() noexcept = default;
  constexpr explicit InternedDimName(std::uint32_t id,
                                     std::string_view name) noexcept
      : id_(id), name_(name) {}

  [[nodiscard]] constexpr std::uint32_t id() const noexcept { return id_; }
  [[nodiscard]] constexpr std::string_view name() const noexcept {
    return name_;
  }
  [[nodiscard]] constexpr std::string_view string() const noexcept {
    return name_;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return id_ == 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return id_ != 0;
  }

  constexpr bool operator==(const InternedDimName &other) const noexcept {
    return id_ == other.id_;
  }
  constexpr auto operator<=>(const InternedDimName &other) const noexcept {
    return id_ <=> other.id_;
  }

private:
  std::uint32_t id_{0};
  std::string_view name_;
};

/**
 * @class DimensionRegistry
 * @brief Centralized registry managing dimension name interning and
 *        (store + name) -> cellid mapping across ZigZag manifolds and stores.
 */
class DimensionRegistry {
public:
  static DimensionRegistry &instance() noexcept;

  DimensionRegistry(const DimensionRegistry &)            = delete;
  DimensionRegistry &operator=(const DimensionRegistry &) = delete;

  /// Intern a dimension string name for fast O(1) comparison.
  InternedDimName intern(std::string_view name);

  /// Return the interned name if already interned, or empty if not.
  [[nodiscard]] InternedDimName findInterned(std::string_view name) const;

  /**
   * @brief Given a manifold and a dimension name, return the cellid of the dim
   *        cell in that store, creating it if it doesn't already exist.
   */
  DimRef getOrCreate(Manifold &manifold, std::string_view name);
  DimRef getOrCreate(Manifold &manifold, InternedDimName dimName);

  /**
   * @brief Overload taking Store explicitly along with Manifold.
   */
  DimRef getOrCreate(xanadu::Store &store, Manifold &manifold,
                     std::string_view name);
  DimRef getOrCreate(xanadu::Store &store, Manifold &manifold,
                     InternedDimName dimName);

  /**
   * @brief Overload taking Store and an active MicroversionId head.
   */
  DimRef getOrCreate(xanadu::Store &store, xanadu::MicroversionId &head,
                     Manifold &manifold, std::string_view name);
  DimRef getOrCreate(xanadu::Store &store, xanadu::MicroversionId &head,
                     Manifold &manifold, InternedDimName dimName);

  /**
   * @brief Lookup an existing dimension cell in the given store without
   * creating.
   *        Returns noCell if it does not exist.
   */
  [[nodiscard]] DimRef get(const xanadu::Store &store,
                           std::string_view name) const;
  [[nodiscard]] DimRef get(const xanadu::Store &store,
                           InternedDimName dimName) const;

  /**
   * @brief Lookup an existing dimension cell in the manifold.
   */
  [[nodiscard]] DimRef get(const Manifold &manifold,
                           std::string_view name) const;
  [[nodiscard]] DimRef get(const Manifold &manifold,
                           InternedDimName dimName) const;

  /**
   * @brief Register or update the mapping for a store and dimension.
   */
  void registerDim(const xanadu::Store &store, InternedDimName dimName,
                   DimRef cell);
  void registerDim(const xanadu::Store &store, std::string_view name,
                   DimRef cell);

  /**
   * @brief Purge all cached dimension mappings for a store (e.g. on Store
   * destruction).
   */
  void unregisterStore(const xanadu::Store *store) noexcept;

  /**
   * @brief Clear all cached store mappings and interned strings.
   */
  void clear() noexcept;

private:
  DimensionRegistry()  = default;
  ~DimensionRegistry() = default;

  mutable std::mutex mutex_;

  // Intern pool: deque guarantees pointer and string_view stability.
  std::deque<std::string> internPool_;
  std::unordered_map<std::string_view, std::uint32_t> nameToId_;

  // Map from (store + dimId) to cellid.
  std::unordered_map<const xanadu::Store *,
                     std::unordered_map<std::uint32_t, DimRef>>
      storeDims_;
};

} // namespace zigzag

template <> struct std::hash<zigzag::InternedDimName> {
  std::size_t operator()(const zigzag::InternedDimName &n) const noexcept {
    return std::hash<std::uint32_t>{}(n.id());
  }
};

template <>
struct std::formatter<zigzag::InternedDimName>
    : std::formatter<std::string_view> {
  auto format(const zigzag::InternedDimName &n, auto &ctx) const {
    return std::formatter<std::string_view>::format(n.name(), ctx);
  }
};

#endif // COMMON_XANADU_ZIGZAG_DIMENSION_REGISTRY_HPP

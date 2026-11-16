#ifndef ZIGZAG_MANIFOLD_LOOKUP_HPP
#define ZIGZAG_MANIFOLD_LOOKUP_HPP

#include "common/xanadu/zigzag/flat_u32_map.hpp"
#include "common/xanadu/zigzag/lookup_prepare_failure.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <utility>
#include <vector>

namespace zigzag::detail {

inline constexpr std::size_t minimumDenseOpCountHint = 4096U;

/// Operation-index mapping: bounded dense storage plus sparse overflow.
class ManifoldOpIndexMap {
public:
  using PrepareFailure = LookupPrepareFailure;
  class PreparedInsert {
  public:
    PreparedInsert(PreparedInsert &&) noexcept            = default;
    PreparedInsert &operator=(PreparedInsert &&) noexcept = default;
    PreparedInsert(const PreparedInsert &)                = delete;
    PreparedInsert &operator=(const PreparedInsert &)     = delete;

  private:
    friend class ManifoldOpIndexMap;
    PreparedInsert(
        const ManifoldOpIndexMap *owner, const std::uint64_t generation,
        const std::uint32_t op, const bool dense, const std::size_t offset,
        std::optional<FlatU32Map<std::uint32_t>::PreparedInsert> &&overflow)
        : owner_(owner), generation_(generation), op_(op), dense_(dense),
          offset_(offset), overflow_(std::move(overflow)) {}

    const ManifoldOpIndexMap *owner_;
    std::uint64_t generation_;
    std::uint32_t op_;
    bool dense_;
    std::size_t offset_;
    std::optional<FlatU32Map<std::uint32_t>::PreparedInsert> overflow_;
  };

  explicit ManifoldOpIndexMap(
      std::pmr::memory_resource *resource = std::pmr::get_default_resource())
      : resource_(resource == nullptr ? std::pmr::get_default_resource()
                                      : resource),
        values_(resource_), present_(resource_), overflow_(resource_) {}

  ManifoldOpIndexMap(const ManifoldOpIndexMap &other)
      : resource_(other.resource_),
        values_(other.values_,
                std::pmr::polymorphic_allocator<std::uint32_t>{resource_}),
        present_(other.present_,
                 std::pmr::polymorphic_allocator<std::uint8_t>{resource_}),
        overflow_(other.overflow_), opCountHint_(other.opCountHint_),
        size_(other.size_), allocationFailure_(other.allocationFailure_),
        generation_(other.generation_) {}
  ManifoldOpIndexMap &operator=(const ManifoldOpIndexMap &other) {
    if (this == &other) return *this;
    std::pmr::vector<std::uint32_t> values(
        other.values_,
        std::pmr::polymorphic_allocator<std::uint32_t>{resource_});
    std::pmr::vector<std::uint8_t> present(
        other.present_,
        std::pmr::polymorphic_allocator<std::uint8_t>{resource_});
    FlatU32Map<std::uint32_t> overflow(other.overflow_, resource_);
    values_.swap(values);
    present_.swap(present);
    overflow_          = std::move(overflow);
    opCountHint_       = other.opCountHint_;
    size_              = other.size_;
    allocationFailure_ = other.allocationFailure_;
    ++generation_;
    return *this;
  }
  ManifoldOpIndexMap(ManifoldOpIndexMap &&other) noexcept
      : resource_(other.resource_), values_(std::move(other.values_)),
        present_(std::move(other.present_)),
        overflow_(std::move(other.overflow_)), opCountHint_(other.opCountHint_),
        size_(std::exchange(other.size_, 0)),
        allocationFailure_(other.allocationFailure_),
        generation_(other.generation_ + 1U) {
    ++other.generation_;
  }
  ManifoldOpIndexMap &operator=(ManifoldOpIndexMap &&other) noexcept {
    if (this != &other) {
      const auto nextGeneration = generation_ + 1U;
      this->~ManifoldOpIndexMap();
      std::construct_at(this, std::move(other));
      generation_ = nextGeneration;
    }
    return *this;
  }

  [[nodiscard]] std::optional<std::size_t> opCountHint() const noexcept {
    return opCountHint_;
  }
  ManifoldOpIndexMap *setOpCountHint(std::optional<std::size_t> hint) noexcept {
    if (opCountHint_ != hint) {
      opCountHint_ = hint;
      ++generation_;
    }
    return this;
  }
  [[nodiscard]] std::optional<std::uint32_t>
  find(std::uint32_t op) const noexcept {
    if (op == 0) return std::nullopt;
    const auto offset = static_cast<std::size_t>(op - 1U);
    if (offset < values_.size() && isPresent(offset)) return values_[offset];
    const auto it = overflow_.find(op);
    return it == overflow_.end() ? std::nullopt
                                 : std::optional<std::uint32_t>{it->second};
  }
  [[nodiscard]] bool contains(std::uint32_t op) const noexcept {
    return find(op).has_value();
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t denseValueCapacity() const noexcept {
    return values_.size();
  }
  [[nodiscard]] std::size_t densePresenceBytes() const noexcept {
    return present_.size();
  }
  [[nodiscard]] std::size_t overflowBucketCapacity() const noexcept {
    return overflow_.capacity();
  }

  [[nodiscard]] std::expected<PreparedInsert, PrepareFailure>
  prepareInsert(std::uint32_t op) noexcept {
    if (op == 0) return std::unexpected{PrepareFailure::InvalidOperationIndex};
    if (contains(op)) return std::unexpected{PrepareFailure::AlreadyPresent};
    const auto bound = denseBound();
    if (!bound) return std::unexpected{bound.error()};
    const auto offset = static_cast<std::size_t>(op - 1U);
    if (static_cast<std::uint64_t>(op) < *bound) {
      if (offset >= values_.size() && !grow(offset + 1U))
        return std::unexpected{allocationFailure_};
      ++generation_;
      return PreparedInsert(this, generation_, op, true, offset, std::nullopt);
    }
    auto token = overflow_.prepareInsert(op);
    if (!token) return std::unexpected{mapFailure(token.error())};
    ++generation_;
    return PreparedInsert(
        this, generation_, op, false, 0,
        std::optional<FlatU32Map<std::uint32_t>::PreparedInsert>{
            std::move(*token)});
  }
  [[nodiscard]] bool commitInsert(PreparedInsert &&token, std::uint32_t op,
                                  std::uint32_t value) noexcept {
    if (token.owner_ != this || token.generation_ != generation_ ||
        token.op_ != op)
      return false;
    if (token.dense_) {
      if (token.offset_ >= values_.size() || isPresent(token.offset_))
        return false;
      values_[token.offset_] = value;
      setPresent(token.offset_);
    } else {
      if (!token.overflow_ ||
          !overflow_.commitInsert(std::move(*token.overflow_), value))
        return false;
    }
    ++size_;
    ++generation_;
    return true;
  }
  bool emplace(std::uint32_t op, std::uint32_t value) noexcept {
    if (contains(op)) return false;
    auto prepared = prepareInsert(op);
    return prepared && commitInsert(std::move(*prepared), op, value);
  }
  friend bool operator==(const ManifoldOpIndexMap &a,
                         const ManifoldOpIndexMap &b) noexcept {
    if (a.size_ != b.size_) return false;
    for (std::size_t offset = 0; offset < a.values_.size(); ++offset) {
      if (a.isPresent(offset)) {
        const auto op = static_cast<std::uint32_t>(offset + 1U);
        if (b.find(op) != a.values_[offset]) return false;
      }
    }
    for (const auto &[op, value] : a.overflow_) {
      if (b.find(op) != value) return false;
    }
    return true;
  }

private:
  [[nodiscard]] std::expected<std::uint64_t, PrepareFailure>
  denseBound() const noexcept {
    const auto n = opCountHint_.value_or(minimumDenseOpCountHint);
    if (static_cast<std::uint64_t>(n) >
        std::numeric_limits<std::uint64_t>::max() / 2U)
      return std::unexpected{PrepareFailure::CapacityOverflow};
    return static_cast<std::uint64_t>(2U) * static_cast<std::uint64_t>(n);
  }
  static PrepareFailure mapFailure(FlatU32MapPrepareFailure reason) noexcept {
    return reason == FlatU32MapPrepareFailure::AllocationUnavailable
               ? PrepareFailure::AllocationUnavailable
               : PrepareFailure::CapacityOverflow;
  }
  bool grow(std::size_t count) noexcept {
    allocationFailure_ = PrepareFailure::CapacityOverflow;
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
      return false;
    try {
      std::pmr::vector<std::uint32_t> values(resource_);
      std::pmr::vector<std::uint8_t> present(resource_);
      const auto current = values_.size();
      const auto doubled =
          current == 0 ? std::size_t{64}
          : current > std::numeric_limits<std::size_t>::max() / 2U
              ? std::numeric_limits<std::size_t>::max()
              : current * 2U;
      const auto grown = std::max(count, doubled);
      if (grown > std::numeric_limits<std::size_t>::max() - 7U) return false;
      values.resize(grown);
      present.resize((grown + 7U) / 8U);
      std::copy(values_.begin(), values_.end(), values.begin());
      std::copy(present_.begin(), present_.end(), present.begin());
      values_.swap(values);
      present_.swap(present);
      ++generation_;
      return true;
    } catch (const std::bad_alloc &) {
      allocationFailure_ = PrepareFailure::AllocationUnavailable;
    } catch (const std::length_error &) {
      allocationFailure_ = PrepareFailure::CapacityOverflow;
    }
    return false;
  }
  bool isPresent(std::size_t offset) const noexcept {
    return (present_[offset / 8U] & (1U << (offset % 8U))) != 0;
  }
  void setPresent(std::size_t offset) noexcept {
    present_[offset / 8U] |= static_cast<std::uint8_t>(1U << (offset % 8U));
  }
  std::pmr::memory_resource *resource_;
  std::pmr::vector<std::uint32_t> values_;
  std::pmr::vector<std::uint8_t> present_;
  FlatU32Map<std::uint32_t> overflow_;
  std::optional<std::size_t> opCountHint_;
  std::size_t size_{0};
  PrepareFailure allocationFailure_{PrepareFailure::CapacityOverflow};
  std::uint64_t generation_{0};
};

} // namespace zigzag::detail
#endif

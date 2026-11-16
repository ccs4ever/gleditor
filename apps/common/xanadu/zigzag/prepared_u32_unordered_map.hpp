#ifndef XANADU_ZIGZAG_PREPARED_U32_UNORDERED_MAP_HPP
#define XANADU_ZIGZAG_PREPARED_U32_UNORDERED_MAP_HPP

#include "common/xanadu/zigzag/lookup_prepare_failure.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace zigzag::detail {

/// PMR unordered-map control with prepared nodes for allocation-free
/// publication.
template <typename Value> class PreparedU32UnorderedMap {
  using Map = std::pmr::unordered_map<std::uint32_t, Value>;

  static_assert(std::is_nothrow_default_constructible_v<Value>);
  static_assert(std::is_nothrow_copy_assignable_v<Value>);

public:
  using key_type       = std::uint32_t;
  using mapped_type    = Value;
  using value_type     = typename Map::value_type;
  using iterator       = typename Map::iterator;
  using const_iterator = typename Map::const_iterator;
  using node_type      = typename Map::node_type;
  using PrepareFailure = LookupPrepareFailure;

  class PreparedInsert {
  public:
    PreparedInsert(PreparedInsert &&) noexcept = default;
    PreparedInsert &operator=(PreparedInsert &&other) noexcept {
      if (this != &other) {
        std::destroy_at(this);
        std::construct_at(this, std::move(other));
      }
      return *this;
    }
    PreparedInsert(const PreparedInsert &)            = delete;
    PreparedInsert &operator=(const PreparedInsert &) = delete;

  private:
    friend class PreparedU32UnorderedMap<Value>;
    PreparedInsert(const PreparedU32UnorderedMap *owner,
                   const std::uint64_t generation, const std::uint32_t key,
                   node_type &&node) noexcept
        : owner_(owner), generation_(generation), key_(key),
          node_(std::move(node)) {}

    const PreparedU32UnorderedMap *owner_;
    std::uint64_t generation_;
    std::uint32_t key_;
    node_type node_;
  };

  explicit PreparedU32UnorderedMap(
      std::pmr::memory_resource *resource = std::pmr::get_default_resource())
      : resource_(resource == nullptr ? std::pmr::get_default_resource()
                                      : resource),
        entries_(resource_) {}

  PreparedU32UnorderedMap(const PreparedU32UnorderedMap &other)
      : resource_(other.resource_),
        entries_(other.entries_, typename Map::allocator_type{resource_}),
        generation_(other.generation_), reservedFor_(other.entries_.size()) {}

  PreparedU32UnorderedMap &operator=(const PreparedU32UnorderedMap &other) {
    if (this == &other) return *this;
    Map replacement(other.entries_, typename Map::allocator_type{resource_});
    entries_.swap(replacement);
    reservedFor_ = entries_.size();
    ++generation_;
    return *this;
  }

  PreparedU32UnorderedMap(PreparedU32UnorderedMap &&other) noexcept
      : resource_(other.resource_), entries_(std::move(other.entries_)),
        generation_(other.generation_ + 1U),
        reservedFor_(std::exchange(other.reservedFor_, 0)) {
    ++other.generation_;
  }

  PreparedU32UnorderedMap &operator=(PreparedU32UnorderedMap &&other) noexcept {
    if (this != &other) {
      const auto nextGeneration = generation_ + 1U;
      this->~PreparedU32UnorderedMap();
      std::construct_at(this, std::move(other));
      generation_ = nextGeneration;
    }
    return *this;
  }

  [[nodiscard]] iterator begin() noexcept { return entries_.begin(); }
  [[nodiscard]] iterator end() noexcept { return entries_.end(); }
  [[nodiscard]] const_iterator begin() const noexcept {
    return entries_.begin();
  }
  [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }
  [[nodiscard]] iterator find(const std::uint32_t key) noexcept {
    return entries_.find(key);
  }
  [[nodiscard]] const_iterator find(const std::uint32_t key) const noexcept {
    return entries_.find(key);
  }
  [[nodiscard]] bool contains(const std::uint32_t key) const noexcept {
    return entries_.contains(key);
  }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t capacity() const noexcept {
    return entries_.bucket_count();
  }
  [[nodiscard]] float maxLoadFactor() const noexcept {
    return entries_.max_load_factor();
  }

  /// Reserve a total-entry upper bound without changing logical contents.
  [[nodiscard]] std::expected<PreparedU32UnorderedMap *, PrepareFailure>
  reserveFor(const std::size_t totalEntries) noexcept {
    if (totalEntries <= reservedFor_ || totalEntries <= entries_.size() ||
        static_cast<long double>(totalEntries) <=
            static_cast<long double>(entries_.bucket_count()) *
                entries_.max_load_factor())
      return this;
    if (totalEntries > entries_.max_size())
      return std::unexpected{PrepareFailure::CapacityOverflow};
    const auto current = reservedFor_;
    const auto geometric =
        current == 0
            ? totalEntries
            : std::min(entries_.max_size(), current > entries_.max_size() / 2U
                                                ? entries_.max_size()
                                                : current * 2U);
    const auto target = std::max(totalEntries, geometric);
    try {
      entries_.reserve(target);
      reservedFor_ = target;
      return this;
    } catch (const std::bad_alloc &) {
      return std::unexpected{PrepareFailure::AllocationUnavailable};
    } catch (const std::length_error &) {
      return std::unexpected{PrepareFailure::CapacityOverflow};
    }
  }

  [[nodiscard]] std::expected<PreparedInsert, PrepareFailure>
  prepareInsert(const std::uint32_t key) noexcept {
    if (key == 0) return std::unexpected{PrepareFailure::InvalidOperationIndex};
    if (entries_.contains(key))
      return std::unexpected{PrepareFailure::AlreadyPresent};
    if (entries_.size() == entries_.max_size())
      return std::unexpected{PrepareFailure::CapacityOverflow};

    try {
      // Inserting and immediately extracting in this destination map grows
      // its buckets for the pending node. That exact capacity is retained for
      // the allocation-free commit, including when insertion triggers rehash.
      auto [it, inserted] = entries_.try_emplace(key, Value{});
      if (!inserted) return std::unexpected{PrepareFailure::AlreadyPresent};
      auto node = entries_.extract(it);
      ++generation_;
      return PreparedInsert(this, generation_, key, std::move(node));
    } catch (const std::bad_alloc &) {
      return std::unexpected{PrepareFailure::AllocationUnavailable};
    } catch (const std::length_error &) {
      return std::unexpected{PrepareFailure::CapacityOverflow};
    }
  }

  /// Publish the same-resource node into capacity proven by its preparation.
  [[nodiscard]] bool commitInsert(PreparedInsert &&token,
                                  const Value &value) noexcept {
    if (token.owner_ != this || token.generation_ != generation_ ||
        token.key_ == 0 || token.node_.empty() ||
        token.node_.key() != token.key_)
      return false;
    token.node_.mapped() = value;
    try {
      auto result = entries_.insert(std::move(token.node_));
      if (!result.inserted) {
        token.node_ = std::move(result.node);
        return false;
      }
    } catch (...) {
      // Callers treat failure after successful preflight as an invariant break.
      return false;
    }
    ++generation_;
    return true;
  }

  friend bool operator==(const PreparedU32UnorderedMap &left,
                         const PreparedU32UnorderedMap &right) {
    return left.entries_ == right.entries_;
  }

private:
  std::pmr::memory_resource *resource_;
  Map entries_;
  std::uint64_t generation_{0};
  // Preserve explicit reservations when later calls ask for a smaller bound.
  std::size_t reservedFor_{0};
};

} // namespace zigzag::detail

#endif // XANADU_ZIGZAG_PREPARED_U32_UNORDERED_MAP_HPP

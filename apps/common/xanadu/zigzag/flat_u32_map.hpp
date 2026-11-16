/**
 * @file flat_u32_map.hpp
 * @brief Engine-private open-addressed map for arbitrary uint32 keys.
 *
 * Occupancy is stored separately from keys and values, so every uint32 key is
 * eligible as a key. Manifold's caller separately refuses operation index
 * zero before using its dense op-index mapping.
 */
#ifndef ZIGZAG_FLAT_U32_MAP_HPP
#define ZIGZAG_FLAT_U32_MAP_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace zigzag::detail {

enum class FlatU32MapPrepareFailure : std::uint8_t {
  AlreadyPresent,
  CapacityOverflow,
  AllocationUnavailable,
};

template <class Value> class FlatU32Map {
public:
  using value_type = std::pair<const std::uint32_t, Value>;

private:
  static_assert(std::is_nothrow_default_constructible_v<Value>,
                "empty buckets need an allocation-free default value");
  static_assert(std::is_trivially_copyable_v<Value>,
                "prepared insertion and rehash require allocation-free values");
  static_assert(std::is_nothrow_copy_constructible_v<Value>);
  static_assert(std::is_nothrow_move_constructible_v<value_type>);
  static_assert(std::is_nothrow_destructible_v<value_type>);

  enum class State : std::uint8_t { Empty, Occupied, Deleted };
  static constexpr std::size_t statesPerByte = 4;

  template <bool IsConst> class BasicIterator {
    using Map = std::conditional_t<IsConst, const FlatU32Map, FlatU32Map>;

  public:
    using difference_type   = std::ptrdiff_t;
    using value_type        = FlatU32Map::value_type;
    using iterator_concept  = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;
    using reference =
        std::conditional_t<IsConst, const value_type &, value_type &>;
    using pointer =
        std::conditional_t<IsConst, const value_type *, value_type *>;

    BasicIterator() = default;

    template <bool OtherConst>
      requires(IsConst && !OtherConst)
    BasicIterator(const BasicIterator<OtherConst> &other) noexcept
        : map_(other.map_), index_(other.index_) {}

    [[nodiscard]] reference operator*() const noexcept {
      return map_->entries_[index_];
    }

    [[nodiscard]] pointer operator->() const noexcept {
      return &map_->entries_[index_];
    }

    BasicIterator &operator++() noexcept {
      ++index_;
      skipEmpty();
      return *this;
    }

    BasicIterator operator++(int) noexcept {
      auto previous = *this;
      ++*this;
      return previous;
    }

    template <bool OtherConst>
    [[nodiscard]] bool
    operator==(const BasicIterator<OtherConst> &other) const noexcept {
      return map_ == other.map_ && index_ == other.index_;
    }

  private:
    friend class FlatU32Map;
    template <bool> friend class BasicIterator;

    BasicIterator(Map *map, const std::size_t index) noexcept
        : map_(map), index_(index) {
      skipEmpty();
    }

    void skipEmpty() noexcept {
      while (map_ != nullptr && index_ < map_->entries_.size() &&
             map_->stateAt(index_) != State::Occupied) {
        ++index_;
      }
    }

    Map *map_{nullptr};
    std::size_t index_{0};
  };

public:
  using iterator       = BasicIterator<false>;
  using const_iterator = BasicIterator<true>;

  /// A capability to commit one insertion without allocation. Any intervening
  /// logical mutation or rehash invalidates it; commitInsert then returns
  /// false.
  class PreparedInsert {
    friend class FlatU32Map;
    const FlatU32Map *owner_{nullptr};
    std::uint64_t generation_{0};
    std::size_t slot_{0};
    std::uint32_t key_{0};

    PreparedInsert(const FlatU32Map *owner, const std::uint64_t generation,
                   const std::size_t slot, const std::uint32_t key) noexcept
        : owner_(owner), generation_(generation), slot_(slot), key_(key) {}

  public:
    PreparedInsert(const PreparedInsert &)                = default;
    PreparedInsert &operator=(const PreparedInsert &)     = default;
    PreparedInsert(PreparedInsert &&) noexcept            = default;
    PreparedInsert &operator=(PreparedInsert &&) noexcept = default;
  };

  explicit FlatU32Map(
      std::pmr::memory_resource *resource = std::pmr::get_default_resource())
      : resource_(resource == nullptr ? std::pmr::get_default_resource()
                                      : resource),
        entries_(resource_), states_(resource_) {}

  FlatU32Map(const FlatU32Map &other) : FlatU32Map(other, other.resource_) {}

  FlatU32Map(const FlatU32Map &other, std::pmr::memory_resource *resource)
      : FlatU32Map(other,
                   resource == nullptr ? std::pmr::get_default_resource()
                                       : resource,
                   CloneTag{}) {}

  FlatU32Map &operator=(const FlatU32Map &other) {
    if (this == &other) {
      return *this;
    }
    FlatU32Map replacement(other, resource_, CloneTag{});
    entries_.swap(replacement.entries_);
    states_.swap(replacement.states_);
    size_    = replacement.size_;
    deleted_ = replacement.deleted_;
    ++generation_;
    return *this;
  }

  FlatU32Map(FlatU32Map &&other) noexcept
      : resource_(other.resource_), entries_(resource_), states_(resource_),
        size_(std::exchange(other.size_, 0)),
        deleted_(std::exchange(other.deleted_, 0)),
        generation_(other.generation_ + 1U) {
    ++other.generation_;
    entries_.swap(other.entries_);
    states_.swap(other.states_);
  }

  FlatU32Map &operator=(FlatU32Map &&other) noexcept {
    if (this != &other) {
      const auto nextGeneration = generation_ + 1U;
      this->~FlatU32Map();
      std::construct_at(this, std::move(other));
      generation_ = nextGeneration;
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t capacity() const noexcept {
    return entries_.size();
  }
  [[nodiscard]] static constexpr std::size_t
  maxLiveEntries(const std::size_t capacity) noexcept {
    return (capacity / 3U) * 2U + ((capacity % 3U) * 2U) / 3U;
  }

  [[nodiscard]] iterator begin() noexcept { return {this, 0}; }
  [[nodiscard]] iterator end() noexcept { return {this, entries_.size()}; }
  [[nodiscard]] const_iterator begin() const noexcept { return {this, 0}; }
  [[nodiscard]] const_iterator end() const noexcept {
    return {this, entries_.size()};
  }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }

  [[nodiscard]] iterator find(const std::uint32_t key) noexcept {
    const auto index = findExisting(key);
    return {this, index.value_or(entries_.size())};
  }

  [[nodiscard]] const_iterator find(const std::uint32_t key) const noexcept {
    const auto index = findExisting(key);
    return {this, index.value_or(entries_.size())};
  }

  [[nodiscard]] bool contains(const std::uint32_t key) const noexcept {
    return findExisting(key).has_value();
  }

  /// Ensure insertion can be committed without allocation. Rehash constructs
  /// replacement arrays before swapping, so failure preserves live entries.
  [[nodiscard]] std::expected<PreparedInsert, FlatU32MapPrepareFailure>
  prepareInsert(const std::uint32_t key) noexcept {
    if (findExisting(key)) {
      return std::unexpected{FlatU32MapPrepareFailure::AlreadyPresent};
    }
    if (!ensureInsertCapacity()) {
      return std::unexpected{preparationFailure_};
    }
    const auto position = insertionPosition(key);
    if (!position) {
      return std::unexpected{FlatU32MapPrepareFailure::CapacityOverflow};
    }
    return PreparedInsert{this, generation_, *position, key};
  }

  /// Insert a prepared key without allocation. Value is allocation-free.
  [[nodiscard]] bool commitInsert(PreparedInsert &&prepared,
                                  const Value &value) noexcept {
    if (prepared.owner_ != this || prepared.generation_ != generation_ ||
        prepared.slot_ >= entries_.size()) {
      return false;
    }
    const auto state = stateAt(prepared.slot_);
    if (state == State::Occupied ||
        (state == State::Deleted && deleted_ == 0)) {
      return false;
    }
    std::destroy_at(&entries_[prepared.slot_]);
    std::construct_at(&entries_[prepared.slot_], prepared.key_, value);
    if (state == State::Deleted) {
      --deleted_;
    }
    setState(prepared.slot_, State::Occupied);
    ++size_;
    ++generation_;
    prepared.owner_ = nullptr;
    return true;
  }

  [[nodiscard]] bool erase(const std::uint32_t key) noexcept {
    const auto found = findExisting(key);
    if (!found) {
      return false;
    }
    std::destroy_at(&entries_[*found]);
    std::construct_at(&entries_[*found]);
    setState(*found, State::Deleted);
    --size_;
    ++deleted_;
    ++generation_;
    return true;
  }

  /// Clear live entries while retaining the allocation for reuse.
  void clear() noexcept {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (stateAt(index) == State::Occupied) {
        std::destroy_at(&entries_[index]);
        std::construct_at(&entries_[index]);
      }
      setState(index, State::Empty);
    }
    size_    = 0;
    deleted_ = 0;
    ++generation_;
  }

  friend bool operator==(const FlatU32Map &left, const FlatU32Map &right) {
    if (left.size_ != right.size_) {
      return false;
    }
    for (const auto &[key, value] : left) {
      const auto found = right.find(key);
      if (found == right.end() || found->second != value) {
        return false;
      }
    }
    return true;
  }

private:
  struct CloneTag {};
  FlatU32Map(const FlatU32Map &other, std::pmr::memory_resource *resource,
             CloneTag)
      : resource_(resource),
        entries_(other.entries_,
                 std::pmr::polymorphic_allocator<value_type>{resource_}),
        states_(other.states_,
                std::pmr::polymorphic_allocator<std::uint8_t>{resource_}),
        size_(other.size_), deleted_(other.deleted_),
        generation_(other.generation_) {}

  [[nodiscard]] static std::size_t hash(const std::uint32_t key) noexcept {
    auto value = static_cast<std::uint64_t>(key) + 0x9e3779b97f4a7c15ULL;
    value      = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value      = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return static_cast<std::size_t>(value ^ (value >> 31U));
  }

  [[nodiscard]] static constexpr std::size_t
  stateBytes(const std::size_t capacity) noexcept {
    return (capacity + statesPerByte - 1U) / statesPerByte;
  }

  [[nodiscard]] State stateAt(const std::size_t index) const noexcept {
    const auto byteIndex = index / statesPerByte;
    const auto shift     = (index % statesPerByte) * 2U;
    return static_cast<State>((states_[byteIndex] >> shift) & 0x3U);
  }

  void setState(const std::size_t index, const State state) noexcept {
    const auto byteIndex = index / statesPerByte;
    const auto shift     = (index % statesPerByte) * 2U;
    const auto mask      = static_cast<std::uint8_t>(0x3U << shift);
    states_[byteIndex]   = static_cast<std::uint8_t>(
        (states_[byteIndex] & static_cast<std::uint8_t>(~mask)) |
        (static_cast<std::uint8_t>(state) << shift));
  }

  [[nodiscard]] std::optional<std::size_t>
  findExisting(const std::uint32_t key) const noexcept {
    if (entries_.empty()) {
      return std::nullopt;
    }
    const auto mask = entries_.size() - 1U;
    auto position   = hash(key) & mask;
    for (std::size_t probes = 0; probes < entries_.size(); ++probes) {
      const auto state = stateAt(position);
      if (state == State::Empty) {
        return std::nullopt;
      }
      if (state == State::Occupied && entries_[position].first == key) {
        return position;
      }
      position = (position + 1U) & mask;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t>
  insertionPosition(const std::uint32_t key) const noexcept {
    if (entries_.empty()) {
      return std::nullopt;
    }
    const auto mask = entries_.size() - 1U;
    auto position   = hash(key) & mask;
    std::optional<std::size_t> firstDeleted;
    for (std::size_t probes = 0; probes < entries_.size(); ++probes) {
      const auto state = stateAt(position);
      if (state == State::Deleted && !firstDeleted) {
        firstDeleted = position;
      } else if (state == State::Empty) {
        return firstDeleted.value_or(position);
      }
      position = (position + 1U) & mask;
    }
    return firstDeleted;
  }

  [[nodiscard]] bool ensureInsertCapacity() noexcept {
    preparationFailure_ = FlatU32MapPrepareFailure::CapacityOverflow;
    auto target         = entries_.size();
    if (target == 0) {
      return rehash(initialCapacity);
    }
    const auto maximum = maxLiveEntries(target);
    if (size_ + deleted_ >= maximum) {
      if (size_ < maximum) {
        // Room remains after removing tombstones; rebuild at the same size.
      } else {
        do {
          if (target > std::numeric_limits<std::size_t>::max() / 2U) {
            return false;
          }
          target *= 2U;
        } while (size_ >= maxLiveEntries(target));
      }
    } else if (deleted_ > target / 4U) {
      // Rebuild in place-sized storage to shorten tombstone probe chains.
    } else {
      return true;
    }
    return rehash(target);
  }

  [[nodiscard]] bool rehash(const std::size_t capacity) noexcept {
    try {
      std::pmr::vector<value_type> replacementEntries{
          std::pmr::polymorphic_allocator<value_type>{resource_}};
      std::pmr::vector<std::uint8_t> replacementStates{
          std::pmr::polymorphic_allocator<std::uint8_t>{resource_}};
      if (capacity > replacementEntries.max_size() ||
          stateBytes(capacity) > replacementStates.max_size()) {
        preparationFailure_ = FlatU32MapPrepareFailure::CapacityOverflow;
        return false;
      }
      replacementEntries.resize(capacity);
      replacementStates.resize(stateBytes(capacity), 0);
      for (std::size_t source = 0; source < entries_.size(); ++source) {
        if (stateAt(source) != State::Occupied) {
          continue;
        }
        const auto key  = entries_[source].first;
        const auto mask = capacity - 1U;
        auto position   = hash(key) & mask;
        while (((replacementStates[position / statesPerByte] >>
                 ((position % statesPerByte) * 2U)) &
                0x3U) != static_cast<std::uint8_t>(State::Empty)) {
          position = (position + 1U) & mask;
        }
        std::destroy_at(&replacementEntries[position]);
        std::construct_at(&replacementEntries[position], key,
                          entries_[source].second);
        const auto shift = (position % statesPerByte) * 2U;
        replacementStates[position / statesPerByte] |=
            static_cast<std::uint8_t>(static_cast<std::uint8_t>(State::Occupied)
                                      << shift);
      }
      entries_.swap(replacementEntries);
      states_.swap(replacementStates);
      deleted_ = 0;
      ++generation_;
      return true;
    } catch (const std::bad_alloc &) {
      preparationFailure_ = FlatU32MapPrepareFailure::AllocationUnavailable;
      return false;
    } catch (const std::length_error &) {
      preparationFailure_ = FlatU32MapPrepareFailure::CapacityOverflow;
      return false;
    }
  }

  static constexpr std::size_t initialCapacity = 8;

  std::pmr::memory_resource *resource_;
  std::pmr::vector<value_type> entries_;
  std::pmr::vector<std::uint8_t> states_;
  std::size_t size_{0};
  std::size_t deleted_{0};
  std::uint64_t generation_{0};
  FlatU32MapPrepareFailure preparationFailure_{
      FlatU32MapPrepareFailure::CapacityOverflow};
};

} // namespace zigzag::detail

#endif // ZIGZAG_FLAT_U32_MAP_HPP

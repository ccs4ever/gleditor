/**
 * @file operand_list.hpp
 * @brief Vortex's eight-inline operand sequence with an unbounded spill.
 */
#ifndef COMMON_XANADU_VORTEX_OPERAND_LIST_HPP
#define COMMON_XANADU_VORTEX_OPERAND_LIST_HPP

#include <gleditor/cpp26_inplace_vector.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag::vortex {

/**
 * @brief Contiguous small operand sequence used by the Vortex core.
 *
 * The allocator parameter exists so the spill's allocation-failure guarantee
 * can be tested without replacing global allocation in the test executable.
 * Production uses the default allocator through OperandList.
 */
template <class Allocator = std::allocator<CellRef>> class BasicOperandList {
  static_assert(std::is_empty_v<Allocator>);
  static_assert(std::is_nothrow_copy_constructible_v<Allocator>);
  static_assert(std::allocator_traits<Allocator>::is_always_equal::value);

  using Inline = gleditor::cpp26::inplace_vector<CellRef, 8>;
  using Spill  = std::vector<CellRef, Allocator>;

public:
  using value_type      = CellRef;
  using size_type       = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference       = CellRef &;
  using const_reference = const CellRef &;
  using iterator        = typename std::span<CellRef>::iterator;
  using const_iterator  = typename std::span<const CellRef>::iterator;

  BasicOperandList() = default;

  BasicOperandList(const BasicOperandList &other)
      : inline_(other.inline_), spill_(other.spill_) {}

  BasicOperandList(BasicOperandList &&other) noexcept(
      std::is_nothrow_move_constructible_v<Inline> &&
      std::is_nothrow_move_constructible_v<Spill>)
      : inline_(std::move(other.inline_)), spill_(std::move(other.spill_)) {
    other.resetMovedFrom();
  }

  BasicOperandList &operator=(const BasicOperandList &other) {
    if (this == &other) return *this;
    if (other.usesSpill()) {
      Spill copied(other.spill_); // allocate before changing this object
      inline_.clear();
      spill_ = std::move(copied);
    } else {
      inline_ = other.inline_;
      resetSpill();
    }
    return *this;
  }

  BasicOperandList &operator=(BasicOperandList &&other) noexcept(
      std::is_nothrow_move_assignable_v<Inline> &&
      std::is_nothrow_move_assignable_v<Spill>) {
    if (this == &other) return *this;
    inline_ = std::move(other.inline_);
    spill_  = std::move(other.spill_);
    other.resetMovedFrom();
    return *this;
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] size_type size() const noexcept {
    return usesSpill() ? spill_.size() : inline_.size();
  }

  [[nodiscard]] CellRef *data() noexcept {
    return usesSpill() ? spill_.data() : inline_.data();
  }
  [[nodiscard]] const CellRef *data() const noexcept {
    return usesSpill() ? spill_.data() : inline_.data();
  }

  [[nodiscard]] std::span<CellRef> span() noexcept { return {data(), size()}; }
  [[nodiscard]] std::span<const CellRef> span() const noexcept {
    return {data(), size()};
  }

  [[nodiscard]] iterator begin() noexcept { return span().begin(); }
  [[nodiscard]] iterator end() noexcept { return span().end(); }
  [[nodiscard]] const_iterator begin() const noexcept { return span().begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return span().end(); }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }

  [[nodiscard]] reference operator[](const size_type index) noexcept {
    return data()[index];
  }
  [[nodiscard]] const_reference
  operator[](const size_type index) const noexcept {
    return data()[index];
  }
  [[nodiscard]] reference front() noexcept { return data()[0]; }
  [[nodiscard]] const_reference front() const noexcept { return data()[0]; }
  [[nodiscard]] reference back() noexcept { return data()[size() - 1]; }
  [[nodiscard]] const_reference back() const noexcept {
    return data()[size() - 1];
  }

  void push_back(CellRef value) {
    if (usesSpill()) {
      spill_.push_back(value);
    } else if (inline_.size() < inlineCapacity) {
      inline_.push_back(value);
    } else {
      spillPrefixAndAppend(value);
    }
  }

  void clear() noexcept {
    if (usesSpill())
      spill_.clear();
    else
      inline_.clear();
  }

  friend bool operator==(const BasicOperandList &left,
                         const BasicOperandList &right) {
    return std::ranges::equal(left.span(), right.span());
  }

  static constexpr size_type inlineCapacity = 8;

private:
  [[nodiscard]] bool usesSpill() const noexcept {
    // The first spill always reserves at least nine cells. Capacity is the
    // state bit, so this keeps the production object to inline storage +
    // vector.
    return spill_.capacity() != 0;
  }

  void spillPrefixAndAppend(const CellRef value) {
    Spill next(spill_.get_allocator());
    next.reserve(inlineCapacity + 1);
    next.insert(next.end(), std::make_move_iterator(inline_.begin()),
                std::make_move_iterator(inline_.end()));
    next.push_back(value);
    spill_.swap(next);
    inline_.clear();
  }

  void resetSpill() noexcept {
    Spill empty(spill_.get_allocator());
    spill_.swap(empty);
  }

  void resetMovedFrom() noexcept {
    inline_.clear();
    resetSpill();
  }

  Inline inline_;
  Spill spill_;
};

using OperandList = BasicOperandList<>;

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_OPERAND_LIST_HPP

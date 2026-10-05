/**
 * @file stepped_view.hpp
 * @brief Lazy stepped path ranges and optional filtering range adaptors.
 */
#ifndef GLEDITOR_STEPPED_VIEW_HPP
#define GLEDITOR_STEPPED_VIEW_HPP

#include <cstddef>
#include <iterator>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace gleditor {

/**
 * @class stepped_path_view
 * @brief Lazy forward view over elements produced by calling a stepping
 * function.
 *
 * Generates an element sequence beginning at @p start and advancing by calling
 * @p step(cur) -> std::optional<T> until:
 * - A step produces std::nullopt (end of path).
 * - A self-loop is detected (step produces current element).
 * - A cycle closing back to @p start is detected.
 * - @p limit iterations are reached (guaranteeing cycle termination on lasso
 * paths).
 */
template <typename T, typename StepFn>
class stepped_path_view
    : public std::ranges::view_interface<stepped_path_view<T, StepFn>> {
public:
  class iterator {
  public:
    using value_type       = T;
    using difference_type  = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;

    constexpr iterator() noexcept = default;
    constexpr iterator(const StepFn *step, std::optional<T> start,
                       std::size_t limit) noexcept
        : step_{step}, start_{start}, cur_{start}, limit_{limit} {}

    [[nodiscard]] constexpr const T &operator*() const noexcept {
      return *cur_;
    }

    constexpr iterator &operator++() noexcept {
      if (!cur_ || !step_) {
        cur_ = std::nullopt;
        return *this;
      }
      ++taken_;
      if (taken_ >= limit_) {
        cur_ = std::nullopt;
        return *this;
      }
      auto next = (*step_)(*cur_);
      if (!next || next == cur_ || next == start_) {
        cur_ = std::nullopt;
        return *this;
      }
      cur_ = next;
      return *this;
    }

    constexpr void operator++(int) noexcept { ++(*this); }

    [[nodiscard]] constexpr bool
    operator==(const iterator &other) const noexcept {
      if (!cur_.has_value() && !other.cur_.has_value()) {
        return true;
      }
      return cur_ == other.cur_ && taken_ == other.taken_;
    }

    [[nodiscard]] constexpr bool
    operator==(std::default_sentinel_t) const noexcept {
      return !cur_.has_value();
    }

  private:
    const StepFn *step_{nullptr};
    std::optional<T> start_{std::nullopt};
    std::optional<T> cur_{std::nullopt};
    std::size_t taken_{0};
    std::size_t limit_{static_cast<std::size_t>(-1)};
  };

  constexpr stepped_path_view() noexcept = default;
  constexpr stepped_path_view(
      StepFn step, std::optional<T> start,
      std::size_t limit = static_cast<std::size_t>(-1)) noexcept
      : step_{std::move(step)}, start_{start}, limit_{limit} {}

  [[nodiscard]] constexpr iterator begin() const noexcept {
    return iterator{&step_, start_, limit_};
  }

  [[nodiscard]] static constexpr std::default_sentinel_t end() noexcept {
    return {};
  }

private:
  StepFn step_{};
  std::optional<T> start_{std::nullopt};
  std::size_t limit_{static_cast<std::size_t>(-1)};
};

template <typename T, typename StepFn>
stepped_path_view(StepFn, std::optional<T>,
                  std::size_t = static_cast<std::size_t>(-1))
    -> stepped_path_view<T, StepFn>;

namespace detail {

struct FilterPresentAdaptor {
  template <std::ranges::input_range R> constexpr auto operator()(R &&r) const {
    return std::forward<R>(r) |
           std::views::filter(
               [](const auto &opt) noexcept { return opt.has_value(); }) |
           std::views::transform([](auto &&opt) noexcept -> decltype(auto) {
             return *std::forward<decltype(opt)>(opt);
           });
  }

  template <std::ranges::input_range R>
  friend constexpr auto operator|(R &&r, const FilterPresentAdaptor &adaptor) {
    return adaptor(std::forward<R>(r));
  }
};

} // namespace detail

/// Range adaptor piping a range of std::optional<T> into a range of unwrapped
/// T.
inline constexpr detail::FilterPresentAdaptor filter_present{};

} // namespace gleditor

#endif // GLEDITOR_STEPPED_VIEW_HPP

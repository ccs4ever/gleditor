#include <gleditor/ranges.hpp>
#include <gleditor/stepped_view.hpp>

#include <cassert>
#include <optional>
#include <ranges>

struct UnsizedSentinel {
  const int *end{};

  friend bool operator==(const int *iterator, UnsizedSentinel sentinel) {
    return iterator == sentinel.end;
  }

  friend bool operator==(UnsizedSentinel sentinel, const int *iterator) {
    return iterator == sentinel;
  }
};

using UnsizedRandomAccess =
    std::ranges::subrange<const int *, UnsizedSentinel,
                          std::ranges::subrange_kind::unsized>;

struct SizedRandomAccessNoSubscript {
  const int *first{};
  const int *last{};

  const int *begin() const { return first; }
  const int *end() const { return last; }
  std::size_t size() const { return static_cast<std::size_t>(last - first); }
};

int main() {
  static_assert(std::ranges::random_access_range<UnsizedRandomAccess>);
  static_assert(!std::ranges::sized_range<UnsizedRandomAccess>);
  static_assert(std::ranges::random_access_range<SizedRandomAccessNoSubscript>);
  static_assert(std::ranges::sized_range<SizedRandomAccessNoSubscript>);

  const int values[]{4, 8, 12};
  const UnsizedRandomAccess unsized{values, UnsizedSentinel{values + 3}};
  assert(gleditor::nthOf(unsized, 1) == 8);
  assert(!gleditor::nthOf(unsized, 3));

  const SizedRandomAccessNoSubscript sized{values, values + 3};
  assert(gleditor::nthOf(sized, 2) == 12);
  assert(!gleditor::nthOf(sized, 3));

  const auto step = [](const int value) -> std::optional<int> {
    return value < 3 ? std::optional{value + 1} : std::nullopt;
  };
  using PathView =
      decltype(gleditor::stepped_path_view(step, std::optional{1}));
  static_assert(std::ranges::input_range<PathView>);
  static_assert(!std::ranges::forward_range<PathView>);
  static_assert(std::input_iterator<typename PathView::iterator>);

  const auto view = gleditor::stepped_path_view(step, std::optional{1});
  int count       = 0;
  for (const int value : view) {
    assert(value == ++count);
  }
  assert(count == 3);
}

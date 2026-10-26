#include <cassert>
#include <concepts>
#include <gleditor/ranges.hpp>
#include <gleditor/stepped_view.hpp>
#include <optional>
#include <ranges>
#include <type_traits>

struct UnsizedSentinel {
  const int *last;
  friend bool operator==(const int *it, UnsizedSentinel s) {
    return it == s.last;
  }
  friend bool operator==(UnsizedSentinel s, const int *it) { return it == s; }
};
using UnsizedRandomAccess =
    std::ranges::subrange<const int *, UnsizedSentinel,
                          std::ranges::subrange_kind::unsized>;
static_assert(std::ranges::random_access_range<UnsizedRandomAccess>);
static_assert(!std::ranges::sized_range<UnsizedRandomAccess>);
static_assert(std::ranges::input_range<UnsizedRandomAccess>);
int main() {
  const int values[]{4, 8, 12};
  auto r    = UnsizedRandomAccess{values, UnsizedSentinel{values + 3}};
  auto step = [](int x) -> std::optional<int> {
    return x < 3 ? std::optional{x + 1} : std::nullopt;
  };
  auto view = gleditor::stepped_path_view(step, std::optional{1});
  static_assert(std::ranges::input_range<decltype(view)>);
  static_assert(!std::ranges::forward_range<decltype(view)>);
}

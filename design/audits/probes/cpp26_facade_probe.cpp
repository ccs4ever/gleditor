#include <array>
#include <cassert>
#include <gleditor/cpp26.hpp>
#include <gleditor/cpp26_concat.hpp>
#include <gleditor/cpp26_inplace_vector.hpp>
#include <gleditor/cpp26_span.hpp>
#include <ranges>
#include <span>
#include <vector>
int invoke(gleditor::cpp26::function_ref<int(int)> callback) {
  return callback(3);
}

int main() {
  int total = 0;
  auto add  = [&](int value) { return total += value; };
  assert(invoke(add) == 3);
  assert(invoke([](int value) { return value * 2; }) == 6);
  int n = 7;
  gleditor::cpp26::optional<int &> ref{n};
  assert(ref.has_value());
  *ref = 9;
  assert(n == 9);
  int count = 0;
  for (int &v : ref) {
    ++count;
    assert(v == 9);
  }
  assert(count == 1);
  gleditor::cpp26::optional<int &> empty;
  for (int &v : empty) {
    (void)v;
    assert(false);
  }
  std::array<int, 2> a{1, 2};
  std::array<int, 1> b{3};
  auto joined = gleditor::cpp26::views::concat(std::span<const int>{a},
                                               std::span<const int>{b});
  std::vector<int> got;
  for (int v : joined) got.push_back(v);
  assert((got == std::vector<int>{1, 2, 3}));
  std::span<int> bytes{a};
  assert(gleditor::cpp26::span_at(bytes, 1) == 2);
  bool threw = false;
  try {
    (void)gleditor::cpp26::span_at(bytes, 2);
  } catch (const std::out_of_range &) {
    threw = true;
  }
  assert(threw);
  gleditor::cpp26::inplace_vector<int, 2> fixed;
  fixed.resize(2);
  assert(fixed.size() == 2);
  static_assert(std::ranges::contiguous_range<decltype(fixed)>);
  return 0;
}

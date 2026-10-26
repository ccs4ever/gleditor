#include <ranges>
struct S {
  const int *end;
  friend bool operator==(const int *p, S s) { return p == s.end; }
  friend bool operator==(S s, const int *p) { return p == s; }
};
using R =
    std::ranges::subrange<const int *, S, std::ranges::subrange_kind::unsized>;
static_assert(std::ranges::random_access_range<R>);
static_assert(!std::ranges::sized_range<R>);

// Counts operator new on this thread between a start and a stop. The
// replacement operator new lives in view_animation_test.cpp (one per binary);
// any ui test may count with these.
#ifndef TESTS_UI_ALLOCATION_COUNT_HPP
#define TESTS_UI_ALLOCATION_COUNT_HPP

#include <cstddef>

namespace allocation_count {

inline thread_local bool counting     = false;
inline thread_local std::size_t count = 0;

/// Allocations made on this thread while one of these lives.
class Scope {
public:
  Scope() {
    count    = 0;
    counting = true;
  }
  ~Scope() { counting = false; }
  Scope(const Scope &)            = delete;
  Scope &operator=(const Scope &) = delete;
  [[nodiscard]] static std::size_t made() { return count; }
};

} // namespace allocation_count

#endif // TESTS_UI_ALLOCATION_COUNT_HPP

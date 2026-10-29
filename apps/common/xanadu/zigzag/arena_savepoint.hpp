#pragma once

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag {

class ArenaSavepoint {
public:
  explicit ArenaSavepoint(ArenaManifold &arena)
      : arena_(arena), mark_(arena.mark()) {}

  ArenaSavepoint(const ArenaSavepoint &)            = delete;
  ArenaSavepoint &operator=(const ArenaSavepoint &) = delete;

  ~ArenaSavepoint() {
    if (active_) arena_.release(mark_);
  }

  ArenaSavepoint *rollback() noexcept {
    if (active_) {
      arena_.release(mark_);
      active_ = false;
    }
    return this;
  }

  ArenaSavepoint *commit() noexcept {
    if (active_) {
      arena_.discard(mark_);
      active_ = false;
    }
    return this;
  }

private:
  ArenaManifold &arena_;
  Mark mark_;
  bool active_{true};
};

} // namespace zigzag

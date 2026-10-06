#pragma once

#include <graphemebreak.h>
#include <linebreak.h>
#include <mutex>

namespace gleditor::text::detail {

inline void initializeUnicodeBreaks() {
  // Pagination workers and UI fitting share libunibreak's global tables.
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    init_linebreak();
    init_graphemebreak();
  });
}

} // namespace gleditor::text::detail

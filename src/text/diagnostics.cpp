#include <gleditor/text/diagnostics.hpp>

namespace gleditor::text {
namespace {
thread_local ShapingStatsScope *activeScope{};
} // namespace

ShapingStatsScope::ShapingStatsScope(const bool capture)
    : previous(activeScope), enabled(capture) {
  if (enabled) {
    activeScope = this;
  }
}

ShapingStatsScope::~ShapingStatsScope() {
  if (enabled) {
    activeScope = previous;
  }
}

void detail::recordLayout(const std::size_t bytes) {
  if (activeScope) {
    ++activeScope->collected.layoutCalls;
    activeScope->collected.inputBytes += bytes;
  }
}

void detail::recordHarfBuzz(const bool fallback) {
  if (activeScope) {
    ++activeScope->collected.harfbuzzCalls;
    if (fallback) {
      ++activeScope->collected.fallbackCalls;
    }
  }
}

} // namespace gleditor::text

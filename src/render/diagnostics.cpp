/**
 * @file diagnostics.cpp
 * @brief Implementation of the driver diagnostic sink.
 */
#include <gleditor/render/diagnostics.hpp> // IWYU pragma: associated

#include <format>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace render {

const char *severityName(const DiagnosticSeverity severity) {
  switch (severity) {
  case DiagnosticSeverity::Info:
    return "info";
  case DiagnosticSeverity::Warning:
    return "warning";
  case DiagnosticSeverity::Error:
    return "error";
  }
  return "unknown";
}

void DiagnosticSink::record(const DiagnosticSeverity severity,
                            const std::string_view message) noexcept {
  // Everything below can allocate, and this runs inside a driver callback that
  // has nowhere to report a failure to. Losing a diagnostic is the acceptable
  // outcome; letting an exception escape into the driver's stack is not.
  try {
    std::string text{message};
    bool shouldLog = false;

    {
      const std::lock_guard lock(mutex);
      if (seen.size() < maxRemembered) {
        if (!seen.insert(text).second) {
          // Already reported once. Drivers repeat the same complaint every
          // draw call, and the first one is the useful one.
          return;
        }
      }

      if (DiagnosticSeverity::Error == severity && !errorPending) {
        firstError   = text;
        errorPending = true;
      }

      // Dropping the newest keeps the oldest, which is the one that explains
      // the rest; a driver that has already queued a screenful is not made
      // clearer by the thirty-third message.
      if (undrained.size() < maxUndrained) {
        undrained.push_back(Diagnostic{severity, text});
      }
      shouldLog = logging;
    }

    if (shouldLog) {
      std::cerr << std::format("driver {}: {}\n", severityName(severity), text);
    }
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // Deliberately swallowed: see above.
  }
}

std::vector<Diagnostic> DiagnosticSink::drain() {
  const std::lock_guard lock(mutex);
  return std::exchange(undrained, {});
}

void DiagnosticSink::setStrict(const bool value) {
  const std::lock_guard lock(mutex);
  strictMode = value;
}

bool DiagnosticSink::strict() const {
  const std::lock_guard lock(mutex);
  return strictMode;
}

void DiagnosticSink::setLogging(const bool value) {
  const std::lock_guard lock(mutex);
  logging = value;
}

bool DiagnosticSink::hasError() const {
  const std::lock_guard lock(mutex);
  return errorPending;
}

void DiagnosticSink::raiseIfError(const std::string_view context) {
  std::string message;
  {
    const std::lock_guard lock(mutex);
    if (!errorPending) {
      return;
    }
    message      = firstError;
    errorPending = false;
    firstError.clear();
    if (!strictMode) {
      // Already logged, and already queued for whoever displays diagnostics.
      return;
    }
  }
  // Thrown from the caller's own frame rather than the driver's, so unwinding
  // is defined and the render loop's handler can report it.
  throw std::runtime_error(std::format("{}: {}", context, message));
}

void DiagnosticSink::clear() {
  const std::lock_guard lock(mutex);
  seen.clear();
  undrained.clear();
  firstError.clear();
  errorPending = false;
}

std::size_t DiagnosticSink::distinctCount() const {
  const std::lock_guard lock(mutex);
  return seen.size();
}

} // namespace render

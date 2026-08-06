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

namespace render {

namespace {

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

} // namespace

void DiagnosticSink::record(const DiagnosticSeverity severity,
                            const std::string_view message) noexcept {
  // Everything below can allocate, and this runs inside a driver callback that
  // has nowhere to report a failure to. Losing a diagnostic is the acceptable
  // outcome; letting an exception escape into the driver's stack is not.
  try {
    std::string text{message};

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
    }

    std::cerr << std::format("driver {}: {}\n", severityName(severity), text);
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // Deliberately swallowed: see above.
  }
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
  }
  // Thrown from the caller's own frame rather than the driver's, so unwinding
  // is defined and the render loop's handler can report it.
  throw std::runtime_error(std::format("{}: {}", context, message));
}

void DiagnosticSink::clear() {
  const std::lock_guard lock(mutex);
  seen.clear();
  firstError.clear();
  errorPending = false;
}

std::size_t DiagnosticSink::distinctCount() const {
  const std::lock_guard lock(mutex);
  return seen.size();
}

} // namespace render
// vi: set sw=2 sts=2 ts=2 et:

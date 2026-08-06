/**
 * @file diagnostics.hpp
 * @brief Collection point for driver diagnostics delivered through C callbacks.
 *
 * Both OpenGL's debug output and Vulkan's debug messenger hand messages back
 * through a C function pointer that the driver invokes on its own stack. An
 * exception must not leave such a callback: unwinding through a frame with no
 * C++ exception tables is undefined, and it happens in the middle of a driver
 * call that is left half-finished. The previous OpenGL renderer threw straight
 * out of its callback, which is exactly that bug.
 *
 * The callback therefore only records. The device raises what was recorded
 * later, from its own code at a frame boundary, where throwing is well defined
 * and the render loop's handler can report it.
 */
#ifndef GLEDITOR_RENDER_DIAGNOSTICS_H
#define GLEDITOR_RENDER_DIAGNOSTICS_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace render {

/// How much a driver diagnostic matters.
enum class DiagnosticSeverity : std::uint8_t {
  Info,    ///< Notification or performance note; logged once, never fatal.
  Warning, ///< Suspicious but not incorrect; logged once, never fatal.
  Error,   ///< An API error or undefined behaviour; raised at the next frame
           ///< boundary when strict mode is on.
};

/// Name of a severity, for logs and for anything that displays one.
const char *severityName(DiagnosticSeverity severity);

/// One message as the driver reported it.
struct Diagnostic {
  DiagnosticSeverity severity{};
  std::string message;
};

/**
 * @class DiagnosticSink
 * @brief Thread-safe, allocation-tolerant record of driver diagnostics.
 *
 * Vulkan may invoke its messenger from any thread it likes, so the sink locks.
 * record() is noexcept because its callers cannot cope with an exception; a
 * failure to store a message loses the message rather than propagating.
 */
class DiagnosticSink {
public:
  /**
   * @brief Note one diagnostic.
   *
   * Repeats of a message already seen are dropped, so a driver complaining
   * once per draw call does not bury the log. The first Error is kept for
   * raiseIfError().
   */
  void record(DiagnosticSeverity severity, std::string_view message) noexcept;

  /// True once an Error has been recorded and not yet consumed.
  [[nodiscard]] bool hasError() const;

  /**
   * @brief Take the messages recorded since the last call.
   *
   * This is what lets the application show diagnostics rather than only log
   * them. Because record() deduplicates, polling every frame does not return
   * the same message again and again.
   */
  [[nodiscard]] std::vector<Diagnostic> drain();

  /**
   * @brief Decide whether a recorded error is fatal.
   *
   * Off by default: a driver complaint the editor can survive should be
   * reported to the user, not used to close the editor. Automated runs turn it
   * on, because a render that provoked driver errors has proved nothing however
   * plausible its output looks.
   */
  void setStrict(bool value);
  [[nodiscard]] bool strict() const;

  /**
   * @brief Whether record() also writes each message to the error stream.
   *
   * On by default. Tests that record thousands of messages to reach the caps
   * below turn it off: the log is where a CI failure is read from, and burying
   * it under deliberate noise costs more than the noise is worth.
   */
  void setLogging(bool value);

  /**
   * @brief Consume the first recorded error, throwing it in strict mode.
   *
   * Always clears the pending error, so one bad call cannot keep failing every
   * later frame. Outside strict mode the error has already been logged and
   * queued for drain(), so consuming it here loses nothing.
   *
   * @param context Prefixed to the message, naming the backend that saw it.
   * @throws std::runtime_error when strict mode is on and an error is pending.
   */
  void raiseIfError(std::string_view context);

  /// Forget everything recorded so far.
  void clear();

  /// Number of distinct messages recorded, for tests and diagnostics.
  [[nodiscard]] std::size_t distinctCount() const;

private:
  /// Cap on remembered messages. A driver that emits endlessly varying text
  /// would otherwise grow this without bound; past the cap, deduplication stops
  /// but recording still works.
  static constexpr std::size_t maxRemembered = 256;
  /// Cap on messages waiting to be drained. Nothing guarantees anyone is
  /// draining -- a headless run never displays anything -- so the queue needs a
  /// bound of its own rather than relying on deduplication for one.
  static constexpr std::size_t maxUndrained = 32;

  mutable std::mutex mutex;
  std::unordered_set<std::string> seen;
  std::vector<Diagnostic> undrained;
  std::string firstError;
  bool errorPending{};
  bool strictMode{};
  bool logging{true};
};

} // namespace render

#endif // GLEDITOR_RENDER_DIAGNOSTICS_H
// vi: set sw=2 sts=2 ts=2 et:

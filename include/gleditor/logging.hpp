#ifndef GLEDITOR_LOGGING_HPP
#define GLEDITOR_LOGGING_HPP

#include <memory>
#include <mutex>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

namespace gleditor::logging {

// A named logger keeps diagnostic output on stderr and lets SPDLOG_LEVEL
// enable one subsystem without enabling every debug call in the process.
inline std::shared_ptr<spdlog::logger> category(const char *name) {
  static std::once_flag levelsLoaded;
  std::call_once(levelsLoaded, [] { spdlog::cfg::load_env_levels(); });

  if (auto existing = spdlog::get(name)) {
    return existing;
  }
  // Two first uses can race. spdlog's registry rejects the second name;
  // retrieve the logger registered by the other thread in that case.
  try {
    return spdlog::stderr_logger_mt(name);
  } catch (const spdlog::spdlog_ex &) {
    if (auto existing = spdlog::get(name)) {
      return existing;
    }
    throw;
  }
}

} // namespace gleditor::logging

// The level guard also skips evaluation of costly formatting arguments when
// a category is disabled. Do not use spdlog's compile-time DEBUG/TRACE macros:
// their default build level removes those calls even when SPDLOG_LEVEL enables
// the category at runtime.
// spdlog names the error level `err` and its logging method `error`, so the
// level and the method are passed separately; one name for both is why
// GLEDITOR_LOG_ERROR once expanded to a level that does not exist.
template <typename... Args>
void constexpr log_at(const char *category_name, auto level_enum,
                      spdlog::format_string_t<Args...> fmt, Args &&...args) {
  static const auto gleditorCategoryLogger =
      gleditor::logging::category(category_name);
  if (gleditorCategoryLogger->should_log(level_enum)) {
    gleditorCategoryLogger->log(level_enum, fmt, std::forward<Args>(args)...);
  }
}

template <typename... Args>
void constexpr GLEDITOR_LOG_TRACE(const char *category_name,
                                  spdlog::format_string_t<Args...> fmt,
                                  Args &&...args) {
  log_at(category_name, spdlog::level::trace, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void constexpr GLEDITOR_LOG_DEBUG(const char *category_name,
                                  spdlog::format_string_t<Args...> fmt,
                                  Args &&...args) {
  log_at(category_name, spdlog::level::debug, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void constexpr GLEDITOR_LOG_INFO(const char *category_name,
                                 spdlog::format_string_t<Args...> fmt,
                                 Args &&...args) {
  log_at(category_name, spdlog::level::info, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void constexpr GLEDITOR_LOG_WARN(const char *category_name,
                                 spdlog::format_string_t<Args...> fmt,
                                 Args &&...args) {
  log_at(category_name, spdlog::level::warn, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void constexpr GLEDITOR_LOG_ERROR(const char *category_name,
                                  spdlog::format_string_t<Args...> fmt,
                                  Args &&...args) {
  log_at(category_name, spdlog::level::err, fmt, std::forward<Args>(args)...);
}

#endif // GLEDITOR_LOGGING_HPP

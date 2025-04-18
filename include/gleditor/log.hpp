#ifndef GLEDITOR_LOG_H
#define GLEDITOR_LOG_H

// #include <spdlog/fmt/ostr.h>
// #include <spdlog/logger.h>
// #include <spdlog/spdlog.h>
#include <ostream>

// XXX: transition away from spdlog, as it fails to link with clang+libc++

class Loggable {
protected:
  // std::shared_ptr<spdlog::logger> logger;

  virtual void print(std::ostream &ost) const { ost << "LOG ME!!!"; };

public:
  // Loggable() : logger(spdlog::default_logger()) {}
  virtual ~Loggable() = default;
  friend std::ostream &operator<<(std::ostream &ost, const Loggable &oth) {
    oth.print(ost);
    return ost;
  };
};

/*template <typename T, typename Char>
struct fmt::formatter<T, Char, std::enable_if_t<std::is_base_of_v<Loggable, T>>>
    : fmt::ostream_formatter {};

template <> struct fmt::formatter<Rect> : fmt::formatter<std::string_view> {
  // parse is inherited from formatter<string_view>.

  format_context::iterator format(const Rect &box, format_context &ctx) const;
};*/

#endif // GLEDITOR_LOG_H

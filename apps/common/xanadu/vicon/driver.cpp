/**
 * @file driver.cpp
 * @brief Iteration-1 stub: start() surfaces the front-end's refusal.
 */
#include "common/xanadu/vicon/driver.hpp"

namespace xanadu::vicon {

Driver::Driver() : core_(arena_), compiler_(core_) {}

std::expected<void, DriverError> Driver::start(std::string_view source) {
  auto program = compiler_.compileSource(source);
  if (!program) {
    return std::unexpected(DriverError{.code   = program.error().code,
                                       .detail = program.error().detail,
                                       .loc    = program.error().loc,
                                       .phase  = "compile"});
  }
  program_   = *program;
  cursor_    = program_.entry;
  started_   = true;
  exhausted_ = false;
  return {};
}

std::optional<zigzag::CellRef> Driver::resume() {
  exhausted_ = true;
  return std::nullopt;
}

Driver &
Driver::every(gleditor::cpp26::function_ref<void(std::string_view)> sink) {
  while (auto cell = resume()) sink(render(*cell));
  return *this;
}

std::expected<std::vector<std::string>, DriverError>
Driver::runAll(std::string_view source) {
  auto started = start(source);
  if (!started) return std::unexpected(started.error());
  std::vector<std::string> out;
  every([&out](std::string_view text) { out.emplace_back(text); });
  return out;
}

std::string Driver::render(zigzag::CellRef cell) const {
  return std::visit(
      [](const auto &v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return v;
        } else if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else {
          return std::to_string(v);
        }
      },
      core_.render(cell));
}

std::string Driver::formatError(const DriverError &e) {
  return "vicon: " + e.phase + ": " + std::to_string(e.loc.line) + ":" +
         std::to_string(e.loc.column) + ": " + e.detail;
}

} // namespace xanadu::vicon

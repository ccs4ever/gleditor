/**
 * @file driver.hpp
 * @brief The vicon driver: owns the arena/core/VM, compiles, and resumes the
 *        top-level generator (spec §8.2).
 *
 * resume() answers in the §4.1 currency: an optional CellRef, where nullopt
 * is Icon failure — the sequence ran dry — not an error. Rendering to text
 * happens only at the arena boundary (the same boundary Version::materialize
 * draws).
 */
#ifndef COMMON_XANADU_VICON_DRIVER_HPP
#define COMMON_XANADU_VICON_DRIVER_HPP

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/vicon/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "gleditor/cpp26.hpp"

namespace xanadu::vicon {

struct DriverError {
  CompileError code{CompileError::UnexpectedToken};
  std::string detail;
  SourceLocation loc{};
  std::string phase; ///< "parse" | "compile" | "run"
};

class Driver {
public:
  Driver();

  /// Compile a source string and start its generator. Replaces any
  /// program currently loaded.
  [[nodiscard]] std::expected<void, DriverError>
  start(std::string_view source);

  /// Resume the top-level generator once. nullopt is Icon failure — the
  /// sequence ran dry — not an error.
  [[nodiscard]] std::optional<zigzag::CellRef> resume();

  /// Drive the generator to exhaustion, projecting each result to text
  /// through render() at the arena boundary. `cpp26::function_ref` because
  /// the sink is called and not kept (spec §8.2).
  Driver &
  every(gleditor::cpp26::function_ref<void(std::string_view)> sink);

  /// Convenience: compile and drive to exhaustion, collecting rendered
  /// results. A failing program yields an empty vector, not an error.
  [[nodiscard]] std::expected<std::vector<std::string>, DriverError>
  runAll(std::string_view source);

  /// Render a result cell to text at the arena boundary.
  [[nodiscard]] std::string render(zigzag::CellRef cell) const;

  /// Render a DriverError as "vicon: line:col: Code: detail" (§8.3).
  [[nodiscard]] static std::string formatError(const DriverError &e);

  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return core_; }
  [[nodiscard]] zigzag::vortex::VortexVM &vm() noexcept { return vm_; }

private:
  zigzag::ArenaManifold arena_;
  zigzag::vortex::VortexCore core_;
  Compiler compiler_;
  CompiledProgram program_{};
  zigzag::CellRef cursor_{zigzag::noCell};
  bool started_{false};
  bool exhausted_{false};
};

} // namespace xanadu::vicon

#endif // COMMON_XANADU_VICON_DRIVER_HPP

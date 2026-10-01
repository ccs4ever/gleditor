/**
 * @file cli.hpp
 * @brief Unified CLI option parser and configuration for Xuzz.
 */
#ifndef XUZZ_CLI_HPP
#define XUZZ_CLI_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <argparse/argparse.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/state.hpp>

#include "common/xanadu/microversion.hpp"

namespace xuzz {

enum class ViewMode : std::uint8_t { Unified, XanadocOnly, ZigzagOnly };

[[nodiscard]] constexpr std::string_view
viewModeName(const ViewMode mode) noexcept {
  switch (mode) {
  case ViewMode::Unified:
    return "unified";
  case ViewMode::XanadocOnly:
    return "xanadoc";
  case ViewMode::ZigzagOnly:
    return "zigzag";
  }
  return "unified";
}

[[nodiscard]] inline std::optional<ViewMode>
parseViewMode(std::string_view str) noexcept {
  if (str == "unified") {
    return ViewMode::Unified;
  }
  if (str == "xanadoc" || str == "xudu") {
    return ViewMode::XanadocOnly;
  }
  if (str == "zigzag") {
    return ViewMode::ZigzagOnly;
  }
  return std::nullopt;
}

struct CliOptions {
  ViewMode viewMode{ViewMode::Unified};
  std::string storePath{"xanadoc"};
  std::string slicePath;
  bool rasterMode{false};
  bool quiet{false};
  bool headless{false};
  bool showConfig{false};
  std::string checkAuthorshipPath;

  // Microversions
  xanadu::MicroversionId opening;
  std::string askedVersion;
  std::string alongside;
  std::string publishAs;
  std::vector<xanadu::MicroversionId> read;
  std::vector<xanadu::MicroversionId> background;
  std::vector<std::pair<xanadu::MicroversionId, std::size_t>> extraImports;

  // Storage & Export
  std::string permascrollPath;
  std::string dumpPermascrollPath;
  bool exportOsmic{false};

  // UI state
  bool onionSkin{false};
  bool mapVisible{false};
  bool pouchOpen{false};
  bool telescopeVisible{false};
  bool physicsEnabled{false};
  bool noBeams{false};
  bool noSworph{false};
  bool wholePages{false};
  bool hasExplicitStore{false};

  // Media & Widgets
  std::vector<std::string> audioMrls;
  std::vector<std::string> videoMrls;
  std::vector<std::string> aliases;
  std::vector<std::string> compares;

  render::Backend backend{render::Backend::OpenGL};
};

class CliParser {
public:
  static bool wantsEveryOption(int argc, const char *const *argv);
  static void buildParser(argparse::ArgumentParser &parser,
                          bool detailed = false);
  static std::optional<CliOptions> parse(argparse::ArgumentParser &parser,
                                         AppStateRef state, int argc,
                                         char **argv);
};

} // namespace xuzz

#endif // XUZZ_CLI_HPP

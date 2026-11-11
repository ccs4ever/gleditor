#ifndef COMMON_XANADU_VORTEX_STDLIB_IMAGE_HPP
#define COMMON_XANADU_VORTEX_STDLIB_IMAGE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace zigzag::vortex {

enum class StdlibMode { ImageThenCpp, Image, Cpp };

struct StdlibPolicy {
  std::optional<StdlibMode> mode;
  std::optional<std::filesystem::path> imageDir;
  std::optional<bool> localImage;
};

struct ResolvedStdlibPolicy {
  StdlibMode mode{StdlibMode::ImageThenCpp};
  std::filesystem::path imageDir;
  bool localImage{true};
  bool localExplicit{false};
  std::optional<std::filesystem::path> localDir;
};

[[nodiscard]] std::string_view stdlibModeName(StdlibMode mode) noexcept;
[[nodiscard]] std::optional<StdlibMode>
parseStdlibMode(std::string_view name) noexcept;

[[nodiscard]] std::shared_ptr<const StdlibPolicy> stdlibPolicySnapshot();
std::shared_ptr<const StdlibPolicy> setStdlibPolicy(StdlibPolicy patch);
std::shared_ptr<const StdlibPolicy> setStdlibSource(StdlibMode mode);
std::shared_ptr<const StdlibPolicy>
setStdlibImageDir(std::filesystem::path dir);
std::shared_ptr<const StdlibPolicy> setStdlibLocalImage(bool on);

/// Applies the trusted startup Settings value, unless an explicit or valid
/// environment source has already selected a mode.
std::shared_ptr<const StdlibPolicy>
applyStdlibSettingsSource(std::string_view source);
[[nodiscard]] ResolvedStdlibPolicy resolveStdlibPolicy();
void resetStdlibPolicyForTesting();

enum class StdlibRefusalCause {
  ImageMissing,
  ImageUnreadable,
  ScrollTruncated,
  EpochMismatch,
  FormatRefused,
  MarksOutstanding,
  ChoiceStackLive,
  GraftRefused,
};

class StdlibImageRefused : public std::runtime_error {
public:
  StdlibImageRefused(
      StdlibRefusalCause cause, std::string detail,
      std::filesystem::path dir = {}, std::size_t liveMarkCount = 0,
      std::optional<std::pair<std::string, std::string>> epochs = {});

  StdlibRefusalCause cause;
  std::filesystem::path dir;
  std::string detail;
  std::size_t liveMarkCount;
  std::optional<std::pair<std::string, std::string>> epochs;

  [[nodiscard]] bool operator==(const StdlibImageRefused &other) const noexcept;
};

struct StdlibImageManifest {
  std::string imageEpoch;
  std::int64_t contentVersion{};
  xanadu::MicroversionId generatorHead;
  std::uint64_t expectedScrollBytes{};
};

struct StdlibRoutineImage {
  CellRef module;
  CellRef symbol;
  CellRef entry;
  std::vector<CellRef> inputs;
  std::vector<CellRef> outputs;
};

struct StdlibImage {
  std::shared_ptr<const xanadu::UserPermascroll> permascroll;
  std::shared_ptr<const xanadu::Store> store;
  Manifold manifold;
  StdlibImageManifest manifest;
  std::filesystem::path dir;
  CellRef home;
  CellRef dimensionRegistry;
  CellRef manifestKey;
  CellRef manifestHolder;
  std::vector<CellRef> opcodeCells;
  std::vector<StdlibRoutineImage> routines;
};

[[nodiscard]] std::expected<std::shared_ptr<const StdlibImage>,
                            StdlibImageRefused>
openStdlibImage(const std::filesystem::path &dir);

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_STDLIB_IMAGE_HPP

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/document_id.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/stdlib_image.hpp"
#include "common/xanadu/vortex/stdlib_image_format.hpp"
#include "common/xanadu/vortex/worker_context.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::MicroversionId;
using xanadu::Store;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::VortexStdLib;
using zigzag::vortex::WorkerContext;

constexpr std::int64_t kDefaultContentVersion =
    zigzag::vortex::kStdlibContentVersion;
constexpr std::string_view kPinnedDocumentBytes{"vortex-stdlib-v1", 16};
constexpr std::string_view kPinnedFingerprint =
    "5555555555555555555555555555555555555555";

enum class CommandMode { None, Generate, Verify, Help };

struct Arguments {
  CommandMode mode{CommandMode::None};
  fs::path path;
  bool wholeStdlib{false};
  std::vector<std::string> modules;
  std::int64_t contentVersion{kDefaultContentVersion};
};

std::string usage() {
  return "Usage:\n"
         "  vortex-stdlib-gen --out DIR (--stdlib | --module NAME "
         "[--module NAME ...]) [--content-version N]\n"
         "  vortex-stdlib-gen --verify DIR\n"
         "  vortex-stdlib-gen --help\n";
}

std::expected<std::int64_t, std::string>
parsePositiveInteger(const std::string_view text, const std::string_view flag) {
  std::int64_t value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} ||
      end != text.data() + text.size() || value <= 0) {
    return std::unexpected(std::string(flag) +
                           " requires a positive decimal integer");
  }
  return value;
}

std::expected<Arguments, std::string> parseArguments(const int argc,
                                                     char **argv) {
  Arguments parsed;
  bool haveOut     = false;
  bool haveVerify  = false;
  bool haveVersion = false;
  bool haveStdlib  = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    const auto valueAfter = [&](const std::string_view flag)
        -> std::expected<std::string_view, std::string> {
      if (i + 1 >= argc || std::string_view{argv[i + 1]}.starts_with("--")) {
        return std::unexpected(std::string(flag) + " requires a value");
      }
      return std::string_view{argv[++i]};
    };
    if (arg == "--help") {
      if (argc != 2) {
        return std::unexpected("--help cannot be combined with other options");
      }
      parsed.mode = CommandMode::Help;
    } else if (arg == "--extend") {
      return std::unexpected("--extend is unsupported; complete images only");
    } else if (arg == "--out") {
      if (haveOut) return std::unexpected("--out was supplied more than once");
      const auto value = valueAfter(arg);
      if (!value) return std::unexpected(value.error());
      parsed.path = *value;
      haveOut     = true;
    } else if (arg == "--verify") {
      if (haveVerify) {
        return std::unexpected("--verify was supplied more than once");
      }
      const auto value = valueAfter(arg);
      if (!value) return std::unexpected(value.error());
      parsed.path = *value;
      haveVerify  = true;
    } else if (arg == "--stdlib") {
      if (haveStdlib) {
        return std::unexpected("--stdlib was supplied more than once");
      }
      parsed.wholeStdlib = true;
      haveStdlib         = true;
    } else if (arg == "--module") {
      const auto value = valueAfter(arg);
      if (!value) return std::unexpected(value.error());
      if (value->empty()) return std::unexpected("--module name is empty");
      parsed.modules.emplace_back(*value);
    } else if (arg == "--content-version") {
      if (haveVersion) {
        return std::unexpected("--content-version was supplied more than once");
      }
      const auto value = valueAfter(arg);
      if (!value) return std::unexpected(value.error());
      const auto version = parsePositiveInteger(*value, arg);
      if (!version) return std::unexpected(version.error());
      parsed.contentVersion = *version;
      haveVersion           = true;
    } else {
      return std::unexpected("unknown argument '" + std::string(arg) + "'");
    }
  }

  if (parsed.mode == CommandMode::Help) return parsed;
  if (haveVerify) {
    if (haveOut || haveVersion || haveStdlib || !parsed.modules.empty()) {
      return std::unexpected(
          "--verify cannot be combined with generation selections");
    }
    parsed.mode = CommandMode::Verify;
    return parsed;
  }
  if (!haveOut) return std::unexpected("generation requires --out DIR");
  if (haveStdlib && !parsed.modules.empty()) {
    return std::unexpected("--stdlib and --module selections cannot be mixed");
  }
  if (!haveStdlib && parsed.modules.empty()) {
    return std::unexpected("select --stdlib or one or more --module values");
  }
  parsed.mode = CommandMode::Generate;
  return parsed;
}

std::expected<std::vector<std::string>, std::string>
canonicalSelection(const Arguments &args) {
  const auto canonical = VortexStdLib::canonicalModuleOrder();
  if (args.wholeStdlib) {
    return canonical | std::views::transform([](const std::string_view name) {
             return std::string(name);
           }) |
           std::ranges::to<std::vector>();
  }

  std::unordered_set<std::string> requested;
  for (const auto &name : args.modules) {
    if (!requested.insert(name).second) {
      return std::unexpected("duplicate module selection '" + name + "'");
    }
    if (std::ranges::find(canonical, name) == canonical.end()) {
      return std::unexpected("unknown module '" + name + "'");
    }
  }
  if (requested.contains("std:goal") && !requested.contains("std:logic")) {
    return std::unexpected("module 'std:goal' requires 'std:logic'");
  }
  std::vector<std::string> ordered;
  for (const auto name : canonical) {
    if (requested.contains(std::string(name))) ordered.emplace_back(name);
  }
  return ordered;
}

std::expected<void, std::string> checkOutputPath(const fs::path &path,
                                                 bool &existsAtCheck) {
  if (path.empty() || path.filename().empty()) {
    return std::unexpected(
        "--out must name a directory, not a filesystem root");
  }
  std::error_code error;
  const bool exists = fs::exists(path, error);
  if (error) {
    return std::unexpected("cannot inspect output directory '" + path.string() +
                           "': " + error.message());
  }
  existsAtCheck = exists;
  if (!exists) return {};
  if (!fs::is_directory(path, error) || error) {
    return std::unexpected("output path is not a directory: '" + path.string() +
                           "'");
  }
  if (!fs::is_empty(path, error) || error) {
    return std::unexpected("output directory is not empty: '" + path.string() +
                           "'");
  }
  return {};
}

template <typename Key>
std::expected<void, std::string> setPinnedPublicKey(Key &key) {
  if constexpr (std::ranges::range<Key>) {
    using Byte = std::ranges::range_value_t<Key>;
    std::ranges::fill(key, static_cast<Byte>(0x5a));
  } else if constexpr (requires(Key &value) { value.bytes; }) {
    using Byte = std::ranges::range_value_t<decltype(key.bytes)>;
    std::ranges::fill(key.bytes, static_cast<Byte>(0x5a));
  } else {
    return std::unexpected(
        "internal error: unsupported fixed public-key representation");
  }
  return {};
}

std::expected<std::shared_ptr<xanadu::UserPermascroll>, std::string>
makeMemoryPermascroll() {
  xanadu::UserPermascroll::Config config;
  config.storageDir.clear();
  const auto fingerprint =
      xanadu::identity::Fingerprint::fromString(kPinnedFingerprint);
  if (!fingerprint) {
    return std::unexpected("internal error: invalid pinned author fingerprint");
  }
  config.masterIdentity = *fingerprint;
  if (const auto pinned = setPinnedPublicKey(config.deviceKeys.publicKey);
      !pinned) {
    return std::unexpected(pinned.error());
  }
  return std::make_shared<xanadu::UserPermascroll>(std::move(config));
}

struct ImageReport {
  std::string physicalHead;
  std::vector<std::string> modules;
  std::size_t operations{};
  std::size_t cells{};
  std::uint64_t scrollBytes{};
  std::int64_t contentVersion{};
};

std::expected<ImageReport, std::string> verifyImage(const fs::path &directory) {
  if (fs::exists(directory / "publication-state")) {
    return std::unexpected("publication-state/ is forbidden in a stdlib image");
  }
  // --verify runs the same loader xuzz will: an artifact that cannot pass
  // openStdlibImage is not publishable, and the check stays honest as the
  // loader's validation grows.
  const auto opened = zigzag::vortex::openStdlibImage(directory);
  if (!opened) {
    return std::unexpected("stdlib image refused by the runtime loader: " +
                           std::string{opened.error().what()});
  }
  const auto &image = *opened;

  ImageReport report{.physicalHead   = image->manifest.generatorHead.str(),
                     .operations     = image->store->opCount(),
                     .cells          = image->manifold.cellCount(),
                     .scrollBytes    = image->permascroll->size(),
                     .contentVersion = image->manifest.contentVersion};

  const auto stdlibDim =
      image->manifold.dimensionNamed("d.stdlib", *image->store);
  if (!stdlibDim) {
    return std::unexpected("stdlib image lacks a registered d.stdlib");
  }
  std::unordered_set<std::string> moduleNames;
  for (const CellRef module :
       zigzag::rankAfter(image->manifold, image->home, *stdlibDim)) {
    const std::string name = image->manifold.textOf(module, *image->store);
    if (!moduleNames.insert(name).second ||
        std::ranges::find(VortexStdLib::canonicalModuleOrder(), name) ==
            VortexStdLib::canonicalModuleOrder().end()) {
      return std::unexpected(
          "stdlib rank contains unknown or repeated module '" + name + "'");
    }
    report.modules.push_back(name);
  }
  if (report.modules.empty()) {
    return std::unexpected("stdlib rank contains no modules");
  }

  WorkerContext manualContext(&image->manifold, image->store.get(),
                              VortexStdLib::Bootstrap::Manual);
  const auto rebound = manualContext.stdlib.rebindInstance();
  if (!rebound) {
    return std::unexpected(
        "Manual stdlib rebinding refused the folded image: " + rebound.error());
  }
  if (image->store->hasPendingMetadata()) {
    return std::unexpected("loaded image has pending edition metadata");
  }
  return report;
}

std::expected<void, std::string>
buildImage(const Arguments &args, const std::vector<std::string> &modules,
           Store &store) {
  if (args.wholeStdlib) {
    WorkerContext context;
    const auto exported =
        context.stdlib.exportImageToStore(store, args.contentVersion);
    if (!exported) {
      return std::unexpected("full stdlib export failed: " + exported.error());
    }
  } else {
    WorkerContext context(nullptr, nullptr, VortexStdLib::Bootstrap::Manual);
    if (const auto begun = context.stdlib.beginImageOps(); !begun) {
      return std::unexpected(begun.error());
    }
    for (const auto &name : modules) {
      const auto built = context.stdlib.buildModule(name);
      if (!built) return std::unexpected(built.error());
    }
    if (const auto ended = context.stdlib.endImageManifest(); !ended) {
      return std::unexpected(ended.error());
    }
    if (const auto rebound = context.stdlib.rebindInstance(); !rebound) {
      return std::unexpected("selected modules cannot rebind safely: " +
                             rebound.error());
    }
    const auto exported =
        context.stdlib.exportImageToStore(store, args.contentVersion);
    if (!exported) {
      return std::unexpected("selected module export failed: " +
                             exported.error());
    }
  }
  if (store.hasPendingMetadata()) {
    return std::unexpected(
        "export left pending edition or annotation metadata");
  }
  return {};
}

void cleanupStagingDirectory(const fs::path &staging) {
  std::error_code ignored;
  fs::remove(staging / "store" / "ops.nodes", ignored);
  fs::remove(staging / "store" / "store.tables", ignored);
  fs::remove(staging / "store", ignored);
  fs::remove(staging / "permascroll" / "active.primedia", ignored);
  fs::remove(staging / "permascroll", ignored);
  fs::remove(staging, ignored);
}

std::expected<void, std::string>
writeArtifact(const fs::path &target, Store &store,
              const std::shared_ptr<xanadu::UserPermascroll> &permascroll,
              const bool targetInitiallyExisted) {
  const fs::path parent =
      target.has_parent_path() ? target.parent_path() : fs::path{"."};
  std::error_code error;
  if (!fs::is_directory(parent, error) || error) {
    return std::unexpected("output parent directory does not exist: '" +
                           parent.string() + "'");
  }
  const fs::path staging =
      parent / ("." + target.filename().string() + ".vortex-stdlib-gen-stage");
  if (fs::exists(staging, error) || error) {
    return std::unexpected("staging path already exists: '" + staging.string() +
                           "'");
  }
  if (!fs::create_directory(staging, error) || error) {
    return std::unexpected("cannot create staging directory '" +
                           staging.string() + "': " + error.message());
  }
  const auto fail = [&](std::string message) {
    cleanupStagingDirectory(staging);
    return std::expected<void, std::string>{
        std::unexpected(std::move(message))};
  };

  try {
    store.save((staging / "store").string());
  } catch (const std::exception &exception) {
    return fail("cannot save generated Store: " +
                std::string(exception.what()));
  }
  if (!permascroll->flush()) {
    return fail("cannot flush the in-memory permascroll");
  }
  const fs::path permascrollDir = staging / "permascroll";
  if (!fs::create_directory(permascrollDir, error) || error) {
    return fail("cannot create artifact permascroll directory: " +
                error.message());
  }
  const fs::path scrollPath = permascrollDir / "active.primedia";
  {
    std::ofstream output(scrollPath, std::ios::binary | std::ios::trunc);
    const auto bytes = permascroll->bytes();
    if (!output || (bytes.size() != 0 &&
                    !output.write(bytes.data(), static_cast<std::streamsize>(
                                                    bytes.size())))) {
      return fail("cannot write artifact permascroll bytes");
    }
  }

  const auto checked = verifyImage(staging);
  if (!checked)
    return fail("generated image verification failed: " + checked.error());
  const bool targetExisted = fs::exists(target, error);
  if (error) return fail("cannot recheck output directory: " + error.message());
  if (targetExisted) {
    if (!targetInitiallyExisted) {
      return fail(
          "output path appeared during generation; refusing to replace '" +
          target.string() + "'");
    }
    if (!fs::is_directory(target, error) || error ||
        !fs::is_empty(target, error) || error) {
      return fail("output directory changed or is no longer empty: '" +
                  target.string() + "'");
    }
    if (!fs::remove(target, error) || error) {
      return fail("cannot replace the pre-existing empty output directory: " +
                  error.message());
    }
  }
  fs::rename(staging, target, error);
  if (error) {
    return fail("cannot publish generated image at '" + target.string() +
                "': " + error.message());
  }
  return {};
}

int run(const int argc, char **argv) {
  const auto parsed = parseArguments(argc, argv);
  if (!parsed) {
    std::cerr << "vortex-stdlib-gen: " << parsed.error() << "\n" << usage();
    return 2;
  }
  const Arguments &args = *parsed;
  if (args.mode == CommandMode::Help) {
    std::cout << usage();
    return 0;
  }
  (void)zigzag::vortex::setStdlibPolicy(
      {.mode = zigzag::vortex::StdlibMode::Cpp, .localImage = false});
  if (args.mode == CommandMode::Verify) {
    const auto verified = verifyImage(args.path);
    if (!verified) {
      std::cerr << "vortex-stdlib-gen: verification failed: "
                << verified.error() << '\n';
      return 1;
    }
    std::cout << "stdlib image OK: head=" << verified->physicalHead
              << " version=" << verified->contentVersion
              << " operations=" << verified->operations
              << " cells=" << verified->cells
              << " scroll-bytes=" << verified->scrollBytes
              << " manual-rebind=ok modules=";
    for (std::size_t i = 0; i < verified->modules.size(); ++i) {
      if (i != 0) std::cout << ',';
      std::cout << verified->modules[i];
    }
    std::cout << '\n';
    return 0;
  }

  const auto modules = canonicalSelection(args);
  if (!modules) {
    std::cerr << "vortex-stdlib-gen: " << modules.error() << '\n';
    return 2;
  }
  const fs::path target       = fs::absolute(args.path).lexically_normal();
  bool targetInitiallyExisted = false;
  if (const auto checked = checkOutputPath(target, targetInitiallyExisted);
      !checked) {
    std::cerr << "vortex-stdlib-gen: " << checked.error() << '\n';
    return 1;
  }
  const fs::path parent =
      target.has_parent_path() ? target.parent_path() : fs::path{"."};
  if (!fs::is_directory(parent)) {
    std::cerr << "vortex-stdlib-gen: output parent directory does not exist: '"
              << parent.string() << "'\n";
    return 1;
  }

  const auto permascrollResult = makeMemoryPermascroll();
  if (!permascrollResult) {
    std::cerr << "vortex-stdlib-gen: " << permascrollResult.error() << '\n';
    return 1;
  }
  xanadu::DocumentId documentId;
  if (!xanadu::DocumentId::fromBytes(kPinnedDocumentBytes, documentId)) {
    std::cerr
        << "vortex-stdlib-gen: internal error: invalid fixed DocumentId\n";
    return 1;
  }
  auto permascroll = *permascrollResult;
  Store store(permascroll, documentId);
  if (const auto built = buildImage(args, *modules, store); !built) {
    std::cerr << "vortex-stdlib-gen: " << built.error() << '\n';
    return 1;
  }
  if (const auto written =
          writeArtifact(target, store, permascroll, targetInitiallyExisted);
      !written) {
    std::cerr << "vortex-stdlib-gen: " << written.error() << '\n';
    return 1;
  }
  std::cout << "stdlib image written: " << target.string() << '\n';
  return 0;
}

} // namespace

int main(const int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &exception) {
    std::cerr << "vortex-stdlib-gen: " << exception.what() << '\n';
    return 1;
  }
}

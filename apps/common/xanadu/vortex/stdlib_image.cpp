#include "common/xanadu/vortex/stdlib_image.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <gleditor/paths.hpp>
#include <spdlog/spdlog.h>

#include "common/xanadu/vortex/stdlib_image_format.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace zigzag::vortex {
namespace {

std::atomic<std::shared_ptr<const StdlibPolicy>> policy{
    std::make_shared<const StdlibPolicy>()};
std::mutex settingsMutex;
std::optional<StdlibMode> settingsMode;

std::optional<std::string> environment(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return std::nullopt;
  return std::string{value};
}

void warnInvalidSource(const std::string_view value,
                       const std::string_view origin) {
  if (const auto logger = spdlog::get("vortex.stdlib")) {
    logger->warn("invalid {} '{}'; expected image, cpp, or imagethencpp",
                 origin, value);
  } else {
    spdlog::warn("[vortex.stdlib] invalid {} '{}'; expected image, cpp, or "
                 "imagethencpp",
                 origin, value);
  }
}

std::optional<StdlibMode> environmentMode() {
  const auto value = environment("GLEDITOR_STDLIB_SOURCE");
  if (!value) return std::nullopt;
  const auto parsed = parseStdlibMode(*value);
  if (!parsed) warnInvalidSource(*value, "GLEDITOR_STDLIB_SOURCE");
  return parsed;
}

std::string_view causeName(const StdlibRefusalCause cause) noexcept {
  switch (cause) {
  case StdlibRefusalCause::ImageMissing:
    return "ImageMissing";
  case StdlibRefusalCause::ImageUnreadable:
    return "ImageUnreadable";
  case StdlibRefusalCause::ScrollTruncated:
    return "ScrollTruncated";
  case StdlibRefusalCause::EpochMismatch:
    return "EpochMismatch";
  case StdlibRefusalCause::FormatRefused:
    return "FormatRefused";
  case StdlibRefusalCause::MarksOutstanding:
    return "MarksOutstanding";
  case StdlibRefusalCause::ChoiceStackLive:
    return "ChoiceStackLive";
  case StdlibRefusalCause::GraftRefused:
    return "GraftRefused";
  }
  return "Unknown";
}

bool isImageCause(const StdlibRefusalCause cause) noexcept {
  return cause == StdlibRefusalCause::ImageMissing ||
         cause == StdlibRefusalCause::ImageUnreadable ||
         cause == StdlibRefusalCause::ScrollTruncated ||
         cause == StdlibRefusalCause::EpochMismatch ||
         cause == StdlibRefusalCause::FormatRefused;
}

std::string refusalMessage(
    const StdlibRefusalCause cause, const std::string &detail,
    const std::filesystem::path &dir, const std::size_t marks,
    const std::optional<std::pair<std::string, std::string>> &epochs) {
  std::string message{causeName(cause)};
  message += ": ";
  message += detail;
  if (epochs) {
    message += " (image epoch ";
    message += epochs->first;
    message += ", engine epoch ";
    message += epochs->second;
    message += ')';
  }
  if (isImageCause(cause)) {
    message += "; image directory: ";
    message += dir.string();
    message += "; set GLEDITOR_STDLIB_DIR to the intended image directory "
               "and retry";
  } else if (cause == StdlibRefusalCause::MarksOutstanding ||
             cause == StdlibRefusalCause::ChoiceStackLive) {
    message += "; ";
    message += std::to_string(marks);
    message += " live mark(s); finish or resume the owning producing flow, "
               "then retry";
  }
  return message;
}

} // namespace

std::string_view stdlibModeName(const StdlibMode mode) noexcept {
  switch (mode) {
  case StdlibMode::ImageThenCpp:
    return "imagethencpp";
  case StdlibMode::Image:
    return "image";
  case StdlibMode::Cpp:
    return "cpp";
  }
  return {};
}

std::optional<StdlibMode>
parseStdlibMode(const std::string_view name) noexcept {
  if (name == "image") return StdlibMode::Image;
  if (name == "cpp") return StdlibMode::Cpp;
  if (name == "imagethencpp") return StdlibMode::ImageThenCpp;
  return std::nullopt;
}

std::shared_ptr<const StdlibPolicy> stdlibPolicySnapshot() {
  return policy.load(std::memory_order_acquire);
}

std::shared_ptr<const StdlibPolicy> setStdlibPolicy(StdlibPolicy patch) {
  auto previous = policy.load(std::memory_order_acquire);
  for (;;) {
    StdlibPolicy next = *previous;
    if (patch.mode) next.mode = patch.mode;
    if (patch.imageDir) next.imageDir = patch.imageDir;
    if (patch.localImage) next.localImage = patch.localImage;
    if (next.mode == previous->mode && next.imageDir == previous->imageDir &&
        next.localImage == previous->localImage)
      return previous;
    auto desired = std::make_shared<const StdlibPolicy>(std::move(next));
    if (policy.compare_exchange_weak(previous, desired,
                                     std::memory_order_release,
                                     std::memory_order_acquire))
      return desired;
  }
}

std::shared_ptr<const StdlibPolicy> setStdlibSource(const StdlibMode mode) {
  return setStdlibPolicy(StdlibPolicy{.mode = mode});
}

std::shared_ptr<const StdlibPolicy>
setStdlibImageDir(std::filesystem::path dir) {
  return setStdlibPolicy(StdlibPolicy{.imageDir = std::move(dir)});
}

std::shared_ptr<const StdlibPolicy> setStdlibLocalImage(const bool on) {
  return setStdlibPolicy(StdlibPolicy{.localImage = on});
}

std::shared_ptr<const StdlibPolicy>
applyStdlibSettingsSource(const std::string_view source) {
  auto current = stdlibPolicySnapshot();
  if (current->mode || environmentMode()) return current;
  const auto parsed = parseStdlibMode(source);
  if (!parsed) {
    warnInvalidSource(source, "Settings vortex.stdlib_source");
    return current;
  }
  std::lock_guard lock{settingsMutex};
  current = stdlibPolicySnapshot();
  if (current->mode || environmentMode()) return current;
  settingsMode = parsed;
  return current;
}

ResolvedStdlibPolicy resolveStdlibPolicy() {
  const auto snapshot = stdlibPolicySnapshot();
  const auto envMode  = environmentMode();
  StdlibMode mode =
      snapshot->mode.value_or(envMode.value_or(StdlibMode::ImageThenCpp));
  if (!snapshot->mode && !envMode) {
    std::lock_guard lock{settingsMutex};
    if (settingsMode) mode = *settingsMode;
  }

  std::filesystem::path imageDir;
  if (snapshot->imageDir) {
    imageDir = *snapshot->imageDir;
  } else if (const auto leaf = environment("GLEDITOR_STDLIB_DIR")) {
    imageDir = *leaf;
  } else if (const auto root = environment("GLEDITOR_ASSET_DIR")) {
    imageDir = std::filesystem::path{*root} / "vortex-stdlib";
#ifdef GLEDITOR_DATADIR
  } else {
    imageDir = std::filesystem::path{GLEDITOR_DATADIR} / "vortex-stdlib";
#else
  } else {
    imageDir = std::filesystem::path{"./assets"} / "vortex-stdlib";
#endif
  }

  const auto localEnvironment = environment("GLEDITOR_STDLIB_LOCAL");
  std::optional<bool> environmentLocal;
  if (localEnvironment) {
    if (*localEnvironment == "0" || *localEnvironment == "1") {
      environmentLocal = *localEnvironment == "1";
    } else if (const auto logger = spdlog::get("vortex.stdlib")) {
      logger->warn("invalid GLEDITOR_STDLIB_LOCAL '{}'; expected 0 or 1",
                   *localEnvironment);
    } else {
      spdlog::warn("[vortex.stdlib] invalid GLEDITOR_STDLIB_LOCAL '{}'; "
                   "expected 0 or 1",
                   *localEnvironment);
    }
  }
  const bool localExplicit =
      snapshot->localImage.has_value() || environmentLocal.has_value();
  const bool localImage = snapshot->localImage.value_or(
      environmentLocal.value_or(mode != StdlibMode::Cpp));

  std::optional<std::filesystem::path> localDir;
  if (localImage) {
    if (const auto overrideDir = environment("GLEDITOR_STDLIB_LOCAL_DIR"))
      localDir = *overrideDir;
    else
      localDir = gleditor::paths::dataPath("xuzz", "vortex-stdlib-local/");
  }
  return ResolvedStdlibPolicy{.mode          = mode,
                              .imageDir      = std::move(imageDir),
                              .localImage    = localImage,
                              .localExplicit = localExplicit,
                              .localDir      = std::move(localDir)};
}

void resetStdlibPolicyForTesting() {
  policy.store(std::make_shared<const StdlibPolicy>(),
               std::memory_order_release);
  std::lock_guard lock{settingsMutex};
  settingsMode.reset();
}

StdlibImageRefused::StdlibImageRefused(
    const StdlibRefusalCause refusalCause, std::string refusalDetail,
    std::filesystem::path imageDir, const std::size_t marks,
    std::optional<std::pair<std::string, std::string>> epochPair)
    : std::runtime_error(refusalMessage(refusalCause, refusalDetail, imageDir,
                                        marks, epochPair)),
      cause{refusalCause}, dir{std::move(imageDir)},
      detail{std::move(refusalDetail)}, liveMarkCount{marks},
      epochs{std::move(epochPair)} {}

bool StdlibImageRefused::operator==(
    const StdlibImageRefused &other) const noexcept {
  return cause == other.cause && dir == other.dir && detail == other.detail &&
         liveMarkCount == other.liveMarkCount && epochs == other.epochs;
}

namespace {

using Refusal = StdlibImageRefused;
using Cause   = StdlibRefusalCause;

[[noreturn]] void refuse(const Cause cause, std::string detail,
                         const std::filesystem::path &dir) {
  throw Refusal{cause, std::move(detail), dir};
}

std::expected<std::vector<CellRef>, std::string>
rankMembers(const Manifold &manifold, const CellRef anchor, const DimRef dim) {
  std::vector<CellRef> members;
  std::unordered_set<CellRef> visited{anchor};
  auto current = anchor;
  while (const auto next =
             manifold.linked(current, dim, zigzag::DimVector::POS)) {
    if (!visited.insert(*next).second)
      return std::unexpected{"cycle in rank at cell " + std::to_string(*next)};
    members.push_back(*next);
    current = *next;
  }
  return members;
}

std::optional<CellRef> valueTarget(const Manifold &manifold,
                                   const CellRef record, const DimRef values) {
  return manifold.linked(record, values, zigzag::DimVector::POS);
}

std::expected<CellRef, std::string>
uniqueNamed(const Manifold &manifold, const std::vector<CellRef> &members,
            const std::string_view name, const xanadu::SpanReader &reader) {
  std::optional<CellRef> found;
  for (const auto cell : members) {
    if (manifold.textOf(cell, reader) != name) continue;
    if (found)
      return std::unexpected{"duplicate record key '" + std::string{name} +
                             "'"};
    found = cell;
  }
  if (!found)
    return std::unexpected{"missing record key '" + std::string{name} + "'"};
  return *found;
}

std::expected<std::uint64_t, std::string>
integerField(const Manifold &manifold, const CellRef record,
             const DimRef values, const xanadu::SpanReader &reader,
             const bool positive) {
  const auto target = valueTarget(manifold, record, values);
  if (!target)
    return std::unexpected{"record '" + manifold.textOf(record, reader) +
                           "' has no value target"};
  const auto number = manifold.asInt64(*target);
  if (!number || *number < 0 || (positive && *number == 0))
    return std::unexpected{"record '" + manifold.textOf(record, reader) +
                           "' must carry a nonnegative Int64"};
  return static_cast<std::uint64_t>(*number);
}

std::expected<std::vector<CellRef>, std::string>
referenceChain(const Manifold &manifold, const CellRef head,
               const DimRef step) {
  auto members = rankMembers(manifold, head, step);
  if (!members) return std::unexpected{members.error()};
  std::vector<CellRef> references;
  references.reserve(members->size());
  for (const auto member : *members) {
    const auto value = manifold.asInt64(member);
    if (!value || *value < 0 ||
        static_cast<std::uint64_t>(*value) >
            std::numeric_limits<CellRef>::max())
      return std::unexpected{"reference member is not a valid Int64 cell ref"};
    const auto ref = static_cast<CellRef>(*value);
    if (!manifold.contains(ref))
      return std::unexpected{"reference member names absent cell " +
                             std::to_string(ref)};
    references.push_back(ref);
  }
  return references;
}

std::expected<void, std::string>
validateManifoldEdges(const Manifold &manifold) {
  for (const auto &slot : manifold.cells()) {
    for (const auto &edge : manifold.dimensionsOf(slot.birthOp)) {
      if (!manifold.contains(edge.dim))
        return std::unexpected{"cell has an absent dimension reference"};
      for (const auto neighbor : {edge.pos, edge.neg}) {
        if (neighbor != zigzag::noCell && !manifold.contains(neighbor))
          return std::unexpected{"cell has an absent link target"};
      }
    }
  }
  return {};
}

bool hasOnlyValueOwner(const Manifold &manifold, const CellRef holder,
                       const CellRef owner, const DimRef values,
                       const DimRef vars) {
  // The holder heads its own private +d.vars rank, so its first member's
  // negward backlink is rank-internal rather than a foreign reference.
  const auto rankSuccessor =
      manifold.linked(holder, vars, zigzag::DimVector::POS);
  for (const auto &slot : manifold.cells()) {
    for (const auto &edge : manifold.dimensionsOf(slot.birthOp)) {
      if (edge.pos == holder && !(slot.birthOp == owner && edge.dim == values))
        return false;
      if (edge.neg == holder && !(edge.dim == vars && rankSuccessor &&
                                  slot.birthOp == *rankSuccessor))
        return false;
    }
  }
  return true;
}

} // namespace

static std::expected<std::shared_ptr<const StdlibImage>, StdlibImageRefused>
openStdlibImageUncached(const std::filesystem::path &dir) {
  try {
    std::error_code ec;
    // exists() separates "not there" (ImageMissing) from "cannot inspect"
    // (ImageUnreadable): is_directory(path, ec) reports ENOENT through ec.
    const bool imageExists = std::filesystem::exists(dir, ec);
    if (ec)
      refuse(Cause::ImageUnreadable,
             "cannot inspect stdlib image directory: " + ec.message(), dir);
    if (!imageExists)
      refuse(Cause::ImageMissing, "stdlib image directory is missing", dir);
    if (!std::filesystem::is_directory(dir))
      refuse(Cause::ImageUnreadable, "stdlib image path is not a directory",
             dir);
    const auto storeDir          = dir / "store";
    const auto permaDir          = dir / "permascroll";
    const bool permascrollExists = std::filesystem::exists(permaDir, ec);
    if (ec)
      refuse(Cause::ImageUnreadable,
             "cannot inspect image permascroll directory: " + ec.message(),
             dir);
    if (!permascrollExists)
      refuse(Cause::ImageMissing, "image permascroll directory is missing",
             dir);
    if (!std::filesystem::is_directory(permaDir))
      refuse(Cause::ImageUnreadable,
             "image permascroll path is not a directory", dir);
    if (!std::filesystem::is_regular_file(permaDir / "active.primedia", ec))
      refuse(Cause::ImageUnreadable,
             "required permascroll file is missing: " +
                 (permaDir / "active.primedia").string(),
             dir);
    for (const auto *name : {"ops.nodes", "store.tables"}) {
      const auto path = storeDir / name;
      if (!std::filesystem::is_regular_file(path, ec))
        refuse(Cause::ImageUnreadable,
               "required store file is missing or unreadable: " + path.string(),
               dir);
    }

    xanadu::UserPermascroll::Config config;
    config.storageDir = permaDir;
    config.readOnly   = true;
    auto permascroll =
        std::make_shared<xanadu::UserPermascroll>(std::move(config));
    auto store = std::make_shared<xanadu::Store>(permascroll);
    store->load(storeDir.string(), true);
    if (!store->isReadOnly() || !permascroll->config().readOnly)
      refuse(Cause::ImageUnreadable,
             "native image resources did not honor read-only open", dir);

    const auto physicalHead = store->segmentedOps().idOf(
        static_cast<std::uint32_t>(store->opCount()));
    if (store->opCount() == 0 || !physicalHead)
      refuse(Cause::FormatRefused, "image store has no physical final head",
             dir);

    std::uint64_t maxSpanEnd = 0;
    for (std::uint32_t index = 1; index <= store->opCount(); ++index) {
      const auto *node = store->getCompactOp(index);
      if (node == nullptr)
        refuse(Cause::ImageUnreadable,
               "operations spool has a missing node at " +
                   std::to_string(index),
               dir);
      const auto span = node->span();
      if (span.empty()) continue;
      if (span.scroll != xanadu::localScroll)
        refuse(Cause::FormatRefused,
               "stdlib image references a non-local permascroll", dir);
      if (span.start > std::numeric_limits<std::uint64_t>::max() - span.length)
        refuse(Cause::FormatRefused, "primedia span end overflows", dir);
      maxSpanEnd = std::max(maxSpanEnd, span.start + span.length);
    }
    if (permascroll->size() < maxSpanEnd)
      refuse(Cause::ScrollTruncated,
             "permascroll ends before a referenced span (" +
                 std::to_string(permascroll->size()) + " bytes, need " +
                 std::to_string(maxSpanEnd) + ")",
             dir);

    auto folded =
        std::make_shared<Manifold>(store->rebuildManifold(*physicalHead));
    if (folded->refusedOps() != 0)
      refuse(Cause::FormatRefused,
             "store fold refused " + std::to_string(folded->refusedOps()) +
                 " Structure operation(s)",
             dir);
    if (auto valid = validateManifoldEdges(*folded); !valid)
      refuse(Cause::FormatRefused, valid.error(), dir);

    const auto home        = folded->home();
    const auto dimsDim     = folded->dimsDimension();
    const auto vars        = folded->dimensionNamed("d.vars", *store);
    const auto values      = folded->dimensionNamed("d.values", *store);
    const auto stdlibDim   = folded->dimensionNamed("d.stdlib", *store);
    const auto step        = folded->dimensionNamed("d.step", *store);
    const auto registryDim = folded->dimensionNamed("d.dims", *store);
    if (!home || !dimsDim || !registryDim ||
        !folded->rankContains(*home, *dimsDim, *registryDim))
      refuse(Cause::FormatRefused,
             "image lacks genesis home or a registered d.dims registry", dir);
    if (!vars || !values)
      refuse(Cause::FormatRefused,
             "store carries no stdlib.image manifest; not a stdlib image", dir);
    if (!stdlibDim || !step)
      refuse(Cause::FormatRefused,
             "stdlib image lacks d.stdlib or d.step schema dimensions", dir);
    const auto dimensions = rankMembers(*folded, *home, *registryDim);
    if (!dimensions) refuse(Cause::FormatRefused, dimensions.error(), dir);
    std::unordered_set<std::string> dimensionNames;
    for (const auto dimension : *dimensions) {
      const auto name = folded->textOf(dimension, *store);
      if (name.empty() || !dimensionNames.insert(name).second)
        refuse(Cause::FormatRefused,
               "empty or duplicate dimension name in d.dims registry", dir);
    }

    const auto homeVars = rankMembers(*folded, *home, *vars);
    if (!homeVars) refuse(Cause::FormatRefused, homeVars.error(), dir);
    const auto manifestKeyResult =
        uniqueNamed(*folded, *homeVars, "stdlib.image", *store);
    if (!manifestKeyResult)
      refuse(Cause::FormatRefused,
             "store carries no valid stdlib.image manifest: " +
                 manifestKeyResult.error(),
             dir);
    if (homeVars->size() != 1)
      refuse(Cause::FormatRefused,
             "image home +d.vars must contain only the stdlib.image key", dir);
    const CellRef manifestKey = *manifestKeyResult;
    const auto holder         = valueTarget(*folded, manifestKey, *values);
    if (!holder || !folded->contains(*holder) ||
        std::ranges::find(*homeVars, *holder) != homeVars->end() ||
        !hasOnlyValueOwner(*folded, *holder, manifestKey, *values, *vars))
      refuse(Cause::FormatRefused,
             "stdlib.image must point to an unranked private holder", dir);
    const auto fields = rankMembers(*folded, *holder, *vars);
    if (!fields) refuse(Cause::FormatRefused, fields.error(), dir);

    const auto epochKey = uniqueNamed(*folded, *fields, "imageEpoch", *store);
    const auto versionKey =
        uniqueNamed(*folded, *fields, "contentVersion", *store);
    const auto headKey = uniqueNamed(*folded, *fields, "generatorHead", *store);
    const auto scrollKey =
        uniqueNamed(*folded, *fields, "expectedScrollBytes", *store);
    const auto opcodeCountKey =
        uniqueNamed(*folded, *fields, "imageOpcodeCount", *store);
    const auto verbCountKey =
        uniqueNamed(*folded, *fields, "imageVerbCount", *store);
    const auto opcodeCellCountKey =
        uniqueNamed(*folded, *fields, "imageOpcodeCellCount", *store);
    const auto opsKey = uniqueNamed(*folded, *fields, "ops", *store);
    for (const auto *result :
         {&epochKey, &versionKey, &headKey, &scrollKey, &opcodeCountKey,
          &verbCountKey, &opcodeCellCountKey, &opsKey})
      if (!*result) refuse(Cause::FormatRefused, result->error(), dir);

    const auto epochTarget = valueTarget(*folded, *epochKey, *values);
    const auto headTarget  = valueTarget(*folded, *headKey, *values);
    if (!epochTarget || !headTarget)
      refuse(Cause::FormatRefused,
             "imageEpoch or generatorHead has no value target", dir);
    StdlibImageManifest manifest;
    manifest.imageEpoch = folded->textOf(*epochTarget, *store);
    const auto contentVersion =
        integerField(*folded, *versionKey, *values, *store, true);
    const auto expectedBytes =
        integerField(*folded, *scrollKey, *values, *store, true);
    const auto opcodeCount =
        integerField(*folded, *opcodeCountKey, *values, *store, true);
    const auto verbCount =
        integerField(*folded, *verbCountKey, *values, *store, true);
    const auto opcodeCellCount =
        integerField(*folded, *opcodeCellCountKey, *values, *store, true);
    for (const auto *result : {&contentVersion, &expectedBytes, &opcodeCount,
                               &verbCount, &opcodeCellCount})
      if (!*result) refuse(Cause::FormatRefused, result->error(), dir);
    manifest.contentVersion      = static_cast<std::int64_t>(*contentVersion);
    manifest.expectedScrollBytes = *expectedBytes;
    try {
      manifest.generatorHead =
          xanadu::MicroversionId::parse(folded->textOf(*headTarget, *store));
    } catch (const std::exception &error) {
      refuse(Cause::FormatRefused,
             "generatorHead is not a MicroversionId: " +
                 std::string{error.what()},
             dir);
    }

    if (permascroll->size() != manifest.expectedScrollBytes)
      refuse(Cause::ScrollTruncated,
             "permascroll size is " + std::to_string(permascroll->size()) +
                 " bytes; manifest requires " +
                 std::to_string(manifest.expectedScrollBytes),
             dir);
    if (manifest.generatorHead != *physicalHead)
      refuse(Cause::FormatRefused,
             "generatorHead does not name the physical final operations head",
             dir);
    if (*opcodeCount != stdlibImageOpcodeCount() ||
        *verbCount != stdlibImageVerbCount())
      refuse(Cause::FormatRefused,
             "manifest opcode or Structure verb count differs from this "
             "engine",
             dir);
    const auto engineEpoch = stdlibImageEpoch();
    if (manifest.imageEpoch != engineEpoch)
      throw Refusal{Cause::EpochMismatch,
                    "stdlib image ABI epoch does not match this engine", dir, 0,
                    std::pair{manifest.imageEpoch, engineEpoch}};

    const auto opsHead = valueTarget(*folded, *opsKey, *values);
    if (!opsHead)
      refuse(Cause::FormatRefused, "ops record has no enumeration head", dir);
    const auto opMembers = rankMembers(*folded, *opsHead, *step);
    if (!opMembers) refuse(Cause::FormatRefused, opMembers.error(), dir);
    std::vector<CellRef> opcodeCells;
    std::unordered_set<CellRef> uniqueOpcodes;
    for (const auto member : *opMembers) {
      const auto rawRef = folded->asInt64(member);
      if (!rawRef || *rawRef < 0 ||
          static_cast<std::uint64_t>(*rawRef) >
              std::numeric_limits<CellRef>::max())
        refuse(Cause::FormatRefused,
               "ops member is not an Int64 cell reference", dir);
      const auto opcode  = static_cast<CellRef>(*rawRef);
      const auto rawKind = folded->asInt64(opcode);
      if (!rawKind || !isRegistrableOpcode(*rawKind))
        refuse(Cause::FormatRefused,
               "ops member names a cell with an unregistrable opcode kind",
               dir);
      if (!uniqueOpcodes.insert(opcode).second)
        refuse(Cause::FormatRefused, "duplicate opcode in ops enumeration",
               dir);
      opcodeCells.push_back(opcode);
    }
    if (opcodeCells.empty())
      refuse(Cause::FormatRefused, "ops enumeration is empty", dir);
    // The count must be verified here, not only at rebind: an image that
    // grafts and commits its manifest and *then* fails rebind wedges the arena
    // — the committed stdlib.image record blocks re-graft and C++ bootstrap.
    if (*opcodeCellCount != static_cast<std::uint64_t>(opcodeCells.size()))
      refuse(Cause::FormatRefused,
             "manifest imageOpcodeCellCount does not match the ops "
             "enumeration",
             dir);

    const auto paramsDim = folded->dimensionNamed("d.params", *store);
    if (!paramsDim)
      refuse(Cause::FormatRefused, "image has no registered d.params", dir);
    const auto modules = rankMembers(*folded, *home, *stdlibDim);
    if (!modules) refuse(Cause::FormatRefused, modules.error(), dir);
    std::vector<StdlibRoutineImage> routines;
    std::unordered_set<std::string> moduleNames;
    for (const auto module : *modules) {
      const auto moduleName = folded->textOf(module, *store);
      if (moduleName.empty() || !moduleNames.insert(moduleName).second)
        refuse(Cause::FormatRefused,
               "empty or duplicate module name in stdlib rank", dir);
      const auto symbols = rankMembers(*folded, module, *vars);
      if (!symbols) refuse(Cause::FormatRefused, symbols.error(), dir);
      std::unordered_set<std::string> symbolNames;
      for (const auto symbol : *symbols) {
        const auto symbolName = folded->textOf(symbol, *store);
        if (symbolName.empty() || !symbolNames.insert(symbolName).second)
          refuse(Cause::FormatRefused,
                 "empty or duplicate symbol in module " + moduleName, dir);
        const auto entry = valueTarget(*folded, symbol, *values);
        const auto signature =
            folded->linked(symbol, *paramsDim, zigzag::DimVector::POS);
        if (!entry || !folded->contains(*entry) || !signature ||
            !folded->contains(*signature))
          refuse(Cause::FormatRefused,
                 "symbol " + moduleName + "/" + symbolName +
                     " lacks an entry or routine signature",
                 dir);
        if (!hasOnlyValueOwner(*folded, *signature, symbol, *paramsDim, *vars))
          refuse(Cause::FormatRefused,
                 "routine signature is not a private parameter holder", dir);
        const auto signatureFields = rankMembers(*folded, *signature, *vars);
        if (!signatureFields)
          refuse(Cause::FormatRefused, signatureFields.error(), dir);
        const auto inputKey =
            uniqueNamed(*folded, *signatureFields, "in", *store);
        const auto outputKey =
            uniqueNamed(*folded, *signatureFields, "out", *store);
        if (!inputKey || !outputKey)
          refuse(Cause::FormatRefused,
                 !inputKey ? inputKey.error() : outputKey.error(), dir);

        auto parameterRefs = [&](const CellRef record)
            -> std::expected<std::vector<CellRef>, std::string> {
          const auto head = valueTarget(*folded, record, *values);
          if (!head)
            return std::unexpected{
                "signature record has no parameter rank head"};
          auto refs = referenceChain(*folded, *head, *step);
          if (!refs) return std::unexpected{refs.error()};
          std::unordered_set<CellRef> unique;
          for (const auto ref : *refs)
            if (!unique.insert(ref).second)
              return std::unexpected{"duplicate parameter reference"};
          return refs;
        };
        auto inputs  = parameterRefs(*inputKey);
        auto outputs = parameterRefs(*outputKey);
        if (!inputs || !outputs)
          refuse(Cause::FormatRefused,
                 !inputs ? inputs.error() : outputs.error(), dir);
        // Predicate records and goal-control data slots are exports, not ops.
        if ((!inputs->empty() || !outputs->empty()) &&
            !uniqueOpcodes.contains(*entry))
          refuse(Cause::FormatRefused,
                 "routine entry is absent from the authoritative ops record",
                 dir);
        routines.push_back(StdlibRoutineImage{.module  = module,
                                              .symbol  = symbol,
                                              .entry   = *entry,
                                              .inputs  = std::move(*inputs),
                                              .outputs = std::move(*outputs)});
      }
    }

    const auto markerDim = folded->dimensionNamed("d.mesh.relocation", *store);
    if (!markerDim)
      refuse(Cause::FormatRefused,
             "image has no registered d.mesh.relocation dimension", dir);
    for (const auto &slot : folded->cells()) {
      const auto marker =
          folded->linked(slot.birthOp, *markerDim, zigzag::DimVector::POS);
      if (!marker) continue;
      if (folded->textOf(*marker, *store) != "cell")
        refuse(Cause::FormatRefused,
               "relocation mark target is not the canonical cell marker", dir);
      std::optional<CellRef> target;
      switch (folded->valueKindOf(slot.birthOp)) {
      case xanadu::ValueKind::Int64: {
        const auto raw = folded->asInt64(slot.birthOp);
        if (!raw || *raw < 0 ||
            static_cast<std::uint64_t>(*raw) >
                std::numeric_limits<CellRef>::max())
          refuse(Cause::FormatRefused,
                 "marked Int64 reference is outside the CellRef range", dir);
        target = static_cast<CellRef>(*raw);
        break;
      }
      case xanadu::ValueKind::OpHandle:
        target = folded->handleTarget(slot.birthOp);
        break;
      default:
        refuse(Cause::FormatRefused,
               "relocation-marked cell is neither Int64 nor OpHandle", dir);
      }
      if (!target || !folded->contains(*target))
        refuse(Cause::FormatRefused,
               "relocation-marked value names an absent target", dir);
    }

    auto image               = std::make_shared<StdlibImage>();
    image->permascroll       = std::move(permascroll);
    image->store             = std::move(store);
    image->manifold          = std::move(*folded);
    image->manifest          = std::move(manifest);
    image->dir               = dir;
    image->home              = *home;
    image->dimensionRegistry = *registryDim;
    image->manifestKey       = manifestKey;
    image->manifestHolder    = *holder;
    image->opcodeCells       = std::move(opcodeCells);
    image->routines          = std::move(routines);
    return std::shared_ptr<const StdlibImage>{std::move(image)};
  } catch (const StdlibImageRefused &error) {
    return std::unexpected{error};
  } catch (const xanadu::OpsSegmentUnreadable &error) {
    return std::unexpected{StdlibImageRefused{
        Cause::ImageUnreadable,
        "OpsSegmentUnreadable: " + std::string{error.what()}, dir}};
  } catch (const xanadu::StoreTablesUnreadable &error) {
    return std::unexpected{StdlibImageRefused{
        Cause::ImageUnreadable,
        "StoreTablesUnreadable: " + std::string{error.what()}, dir}};
  } catch (const std::exception &error) {
    return std::unexpected{StdlibImageRefused{
        Cause::ImageUnreadable,
        "native store or permascroll open failed: " + std::string{error.what()},
        dir}};
  } catch (...) {
    return std::unexpected{StdlibImageRefused{
        Cause::ImageUnreadable, "unknown native image open failure", dir}};
  }
}

namespace {

// The image is a sealed, read-only asset, but every open pays a full store
// fold plus manifold build -- and worker contexts, parallel query batches,
// and command-path host rebuilds each construct a VortexStdLib. Cache the
// shared image per directory, keyed on the three artifact stamps so a
// regenerated image is picked up without a restart.
struct ImageStamp {
  std::filesystem::file_time_type opsWrite{};
  std::filesystem::file_time_type tablesWrite{};
  std::filesystem::file_time_type scrollWrite{};
  std::uintmax_t opsSize{0};
  std::uintmax_t tablesSize{0};
  std::uintmax_t scrollSize{0};
  bool complete{false};
};

ImageStamp stampImageFiles(const std::filesystem::path &dir) {
  ImageStamp stamp;
  std::error_code ec;
  const auto take = [&](const std::filesystem::path &path,
                        std::filesystem::file_time_type &write,
                        std::uintmax_t &size) {
    write = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    size = std::filesystem::file_size(path, ec);
    return !ec;
  };
  stamp.complete =
      take(dir / "store" / "ops.nodes", stamp.opsWrite, stamp.opsSize) &&
      take(dir / "store" / "store.tables", stamp.tablesWrite,
           stamp.tablesSize) &&
      take(dir / "permascroll" / "active.primedia", stamp.scrollWrite,
           stamp.scrollSize);
  return stamp;
}

bool sameStamp(const ImageStamp &a, const ImageStamp &b) {
  return a.opsWrite == b.opsWrite && a.opsSize == b.opsSize &&
         a.tablesWrite == b.tablesWrite && a.tablesSize == b.tablesSize &&
         a.scrollWrite == b.scrollWrite && a.scrollSize == b.scrollSize;
}

struct CacheEntry {
  std::shared_ptr<const StdlibImage> image;
  ImageStamp stamp;
};

// Both are deliberately leaked: a namespace-scope cache would be destroyed at
// exit after DimensionRegistry::instance() (a function-local static built
// during the run), and ~Store -> unregisterStore would then touch a dead
// registry. Leaking keeps every cached image alive until the OS reclaims it.
std::mutex &imageCacheMutex() {
  static auto *mutex = new std::mutex;
  return *mutex;
}

std::unordered_map<std::string, CacheEntry> &imageCache() {
  static auto *cache = new std::unordered_map<std::string, CacheEntry>;
  return *cache;
}

} // namespace

std::expected<std::shared_ptr<const StdlibImage>, StdlibImageRefused>
openStdlibImage(const std::filesystem::path &dir) {
  const auto key   = dir.lexically_normal().generic_string();
  const auto stamp = stampImageFiles(dir);
  if (stamp.complete) {
    std::lock_guard lock(imageCacheMutex());
    if (const auto it = imageCache().find(key);
        it != imageCache().end() && sameStamp(it->second.stamp, stamp)) {
      return it->second.image;
    }
  }
  auto opened = openStdlibImageUncached(dir);
  if (opened && stamp.complete) {
    std::lock_guard lock(imageCacheMutex());
    auto &cache = imageCache();
    // Real deployments resolve one or two directories; the cap keeps test
    // suites that generate hundreds of throwaway images from accumulating
    // every fold in memory. Overflow just reloads on the next open.
    if (cache.size() >= 8) cache.clear();
    cache[key] = CacheEntry{*opened, stamp};
  }
  return opened;
}

} // namespace zigzag::vortex

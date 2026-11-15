#include "common/xanadu/vortex/vortex_vm.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#include "common/xanadu/zigzag/arena_savepoint.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace zigzag::vortex {
namespace {

constexpr std::string_view closureDescriptorTag = "VORTEX_CLOSURE_DESCRIPTOR/2";
constexpr std::string_view closureValueTag      = "VORTEX_CLOSURE_VALUE/1";
constexpr std::string_view frameReferenceRole   = "ref.frame-descriptor";
constexpr std::string_view captureCountRole     = "count.capture-streams";
constexpr std::string_view minFormalRole        = "count.min-formals";
constexpr std::string_view maxFormalRole        = "count.max-formals";
constexpr std::string_view presenceTrueRole     = "ref.presence-true";
constexpr std::string_view presenceFalseRole    = "ref.presence-false";
constexpr std::string_view captureInputRole     = "input.capture";
constexpr std::string_view appliedInputRole     = "input.applied";
constexpr std::string_view contextInputRole     = "input.context";
constexpr std::string_view presenceInputRole    = "input.applied-presence";
constexpr std::string_view valueDescriptorRole  = "ref.descriptor";
constexpr std::string_view environmentStreamRole = "env.stream";
constexpr std::string_view environmentMemberRole = "ref.environment-member";

ClosureDescriptorError closureError(std::string message) {
  return {.message = std::move(message)};
}

std::optional<std::string> boundedText(const ArenaManifold &arena,
                                       const CellRef cell,
                                       const std::size_t byteLimit) {
  if (!arena.contains(cell)) return std::nullopt;
  std::size_t bytes = 0;
  const auto spans  = arena.contentOf(cell);
  if (spans.size() > byteLimit) return std::nullopt;
  for (const auto &span : spans) {
    arena.consumeWork();
    if (span.length > byteLimit - std::min(bytes, byteLimit))
      return std::nullopt;
    bytes += static_cast<std::size_t>(span.length);
  }
  auto text = arena.textOf(cell);
  if (text.size() != bytes) return std::nullopt;
  return text;
}

std::expected<std::optional<CellRef>, ClosureDescriptorError>
boundedMetadataLink(const ArenaManifold &arena, const CellRef cell,
                    const DimRef dimension, const DimVector direction,
                    const std::size_t relationLimit) {
  const auto links = arena.dimensionsOf(cell);
  if (links.size() > relationLimit)
    return std::unexpected(
        closureError("closure metadata cell exceeds its relation limit"));
  std::optional<CellRef> target;
  for (const auto &link : links) {
    arena.consumeWork();
    if (link.dim != dimension) continue;
    const auto neighbor = link.neighbor(direction);
    if (!neighbor) continue;
    if (target)
      return std::unexpected(
          closureError("closure metadata has duplicate directed links"));
    target = *neighbor;
  }
  return target;
}

std::optional<std::string> claimedTag(const VortexCore &core,
                                      const CellRef root,
                                      const std::size_t byteLimit,
                                      const std::size_t relationLimit) {
  const auto &arena    = core.arena();
  const auto dimension = core.dims().runtimeObjectClaim;
  if (!dimension || !arena.contains(root) ||
      arena.valueKindOf(root) != xanadu::ValueKind::None)
    return std::nullopt;
  const auto probe = boundedText(arena, root, byteLimit);
  if (!probe || *probe != zigzag::runtimeObjectProbeText) return std::nullopt;
  if (arena.dimensionsOf(root).size() > relationLimit) return std::nullopt;
  const auto claim = zigzag::classifyRuntimeObjectRoot(
      arena, root, *dimension,
      {.maxRootRelations = relationLimit,
       .maxMappingWork   = byteLimit > SIZE_MAX / 4
                               ? SIZE_MAX
                               : std::max<std::size_t>(4096, byteLimit * 4)});
  if (claim == zigzag::RuntimeObjectClaimResult::FuelExhausted)
    throw std::runtime_error("Vortex evaluation work budget exhausted");
  if (claim != zigzag::RuntimeObjectClaimResult::Claimed) return std::nullopt;
  const auto kindCell = arena.linked(root, *dimension, DimVector::POS);
  if (!kindCell) return std::nullopt;
  const auto text = boundedText(arena, *kindCell, byteLimit);
  if (!text || (*text != closureDescriptorTag && *text != closureValueTag))
    return std::nullopt;
  return text;
}

std::expected<std::vector<CellRef>, ClosureDescriptorError>
closureWing(const VortexCore &core, const CellRef opcode, const DimVector side,
            const std::size_t portLimit, const std::size_t relationLimit) {
  const auto &arena = core.arena();
  auto head =
      boundedMetadataLink(arena, opcode, core.dims().grab, side, relationLimit);
  if (!head) return std::unexpected(head.error());
  auto current = *head;
  std::vector<CellRef> operands;
  std::unordered_set<CellRef> visited;
  while (current) {
    arena.consumeWork();
    if (operands.size() >= portLimit || !visited.insert(*current).second)
      return std::unexpected(closureError(
          "closure operand wing exceeds its port limit or cycles"));
    const auto target = arena.asInt64(*current);
    if (!target || *target < 0 ||
        static_cast<std::uint64_t>(*target) >
            std::numeric_limits<CellRef>::max() ||
        !arena.contains(static_cast<CellRef>(*target)))
      return std::unexpected(
          closureError("closure operand wing requires cell-reference records"));
    operands.push_back(static_cast<CellRef>(*target));
    auto next = boundedMetadataLink(arena, *current, core.dims().step,
                                    DimVector::POS, relationLimit);
    if (!next) return std::unexpected(next.error());
    current = *next;
  }
  return operands;
}

bool closureOperandHasPipeline(const VortexCore &core, const CellRef cell) {
  if (cell == core.home()) return false;
  // Actual data cells may have many dimensions. Charge their inspection
  // without imposing the metadata relation cap on the captured data itself.
  for (const auto &link : core.arena().dimensionsOf(cell)) {
    core.arena().consumeWork();
    if (link.dim == core.dims().spin)
      return !link.neighbor(DimVector::NEG) && link.neighbor(DimVector::POS);
  }
  return false;
}

CellRef makeClaimedRoot(VortexCore &core, const std::string_view tag,
                        const std::size_t metadataLimit) {
  if (tag.size() > metadataLimit ||
      zigzag::runtimeObjectProbeText.size() > metadataLimit)
    throw std::length_error("closure metadata tag exceeds its byte limit");
  const auto dimension = core.dims().runtimeObjectClaim;
  if (!dimension)
    throw std::runtime_error("Vortex runtime-object dimension is unavailable");
  auto &arena        = core.arena();
  const CellRef root = arena.makeCell();
  const auto probe   = arena.intern(zigzag::runtimeObjectProbeText);
  expectWritten(
      arena.setContent(root, std::span<const xanadu::PrimediaSpan>{&probe, 1}));
  expectWritten(arena.setValueBits(root, xanadu::ValueKind::None, 0));
  const CellRef kindCell = arena.makeCell(tag);
  expectWritten(arena.setValueBits(kindCell, xanadu::ValueKind::None, 0));
  expectWritten(arena.link(root, *dimension, DimVector::POS, kindCell));
  return root;
}

void addRecord(VortexCore &core, const CellRef root,
               const std::string_view role, const std::uint64_t value,
               std::optional<CellRef> &tail) {
  auto &arena          = core.arena();
  const CellRef record = arena.makeCell(role);
  expectWritten(arena.setValueBits(record, xanadu::ValueKind::Int64, value));
  expectWritten(arena.link(tail.value_or(root),
                           tail ? core.dims().step : core.dims().grab,
                           DimVector::POS, record));
  tail = record;
}

std::expected<void, ClosureDescriptorError>
validateClosureSpec(const VortexVM &vm, const ClosureDescriptorSpec &spec) {
  const auto fail =
      [](std::string message) -> std::expected<void, ClosureDescriptorError> {
    return std::unexpected(closureError(std::move(message)));
  };
  const auto limits = vm.closureLimits();
  if (limits.maxCaptureStreams > closureStreamCapacity ||
      limits.maxAppliedStreams > closureStreamCapacity ||
      limits.maxMetadataRelations > closureMetadataRelationCapacity)
    return fail("closure limits exceed their fixed capacities");
  const std::size_t minFormalCount =
      spec.minFormalCount.value_or(spec.maxFormalCount);
  if (spec.captureStreamCount > limits.maxCaptureStreams ||
      spec.maxFormalCount > limits.maxAppliedStreams ||
      minFormalCount > spec.maxFormalCount)
    return fail("closure descriptor exceeds its stream-count limit");
  std::optional<FrameDescriptor> frame;
  if (spec.frameDescriptor) {
    const auto parsed = vm.frameDescriptor(*spec.frameDescriptor);
    if (!parsed)
      return fail("closure frame descriptor: " + parsed.error().message);
    frame = *parsed;
  } else if (!spec.frameInputs.empty()) {
    return fail("closure without a frame cannot map frame inputs");
  }
  const std::size_t framePorts = frame ? frame->argumentPorts.size() : 0;
  if (spec.frameInputs.size() != framePorts)
    return fail("closure source map must cover every frame argument port");
  const std::size_t fixedRecords = 5 + (spec.frameDescriptor ? 1 : 0);
  if (limits.maxDescriptorRecords < fixedRecords ||
      spec.frameInputs.size() > limits.maxDescriptorRecords - fixedRecords)
    return fail("closure descriptor exceeds the descriptor-record limit");

  std::vector<bool> captureUsed(spec.captureStreamCount, false);
  std::vector<bool> appliedUsed(spec.maxFormalCount, false);
  std::vector<bool> presenceUsed(spec.maxFormalCount, false);
  for (std::size_t portIndex = 0; portIndex < spec.frameInputs.size();
       ++portIndex) {
    const auto &input       = spec.frameInputs[portIndex];
    const auto portKind     = frame->argumentPorts[portIndex].kind;
    std::vector<bool> *used = nullptr;
    switch (input.source) {
    case ClosureInputSource::Capture:
      if (input.index < captureUsed.size()) used = &captureUsed;
      break;
    case ClosureInputSource::Applied:
      if (input.index < appliedUsed.size()) used = &appliedUsed;
      break;
    case ClosureInputSource::Context:
      if (input.index != 0 || portKind != FramePortKind::Stream)
        return fail("closure context source must map to a Stream port");
      continue;
    case ClosureInputSource::AppliedPresence:
      if (input.index < presenceUsed.size()) used = &presenceUsed;
      break;
    }
    if (used == nullptr)
      return fail("closure source map contains an out-of-range source");
    if ((input.source == ClosureInputSource::AppliedPresence &&
         portKind != FramePortKind::Cell) ||
        (input.source == ClosureInputSource::Context &&
         portKind != FramePortKind::Stream))
      return fail("closure source kind does not match its frame port kind");
    (*used)[input.index] = true;
  }
  if (std::ranges::any_of(captureUsed, [](bool used) { return !used; }) ||
      std::ranges::any_of(appliedUsed, [](bool used) { return !used; }))
    return fail("every closure source stream must be used by the frame");
  for (std::size_t index = 0; index < presenceUsed.size(); ++index) {
    if (presenceUsed[index] &&
        std::ranges::count_if(spec.frameInputs, [index](const auto &input) {
          return input.source == ClosureInputSource::AppliedPresence &&
                 input.index == index;
        }) != 1)
      return fail("closure presence source is mapped more than once");
  }
  return {};
}

bool addWithin(std::size_t &total, const std::size_t amount,
               const std::size_t limit) {
  if (amount > limit - std::min(total, limit)) return false;
  total += amount;
  return true;
}

} // namespace

VortexVM *VortexVM::setClosureLimits(const LogicalClosureLimits limits) {
  if (limits.maxCaptureStreams > closureStreamCapacity ||
      limits.maxAppliedStreams > closureStreamCapacity ||
      limits.maxMetadataRelations > closureMetadataRelationCapacity)
    throw std::invalid_argument("closure limits exceed their fixed capacities");
  closureLimits_ = limits;
  return this;
}

std::expected<CellRef, ClosureDescriptorError>
VortexVM::makeClosureDescriptor(const ClosureDescriptorSpec &spec) try {
  auto &arena = core_.arena();
  if (const auto valid = validateClosureSpec(*this, spec); !valid)
    return std::unexpected(valid.error());
  const auto limits = closureLimits_;
  const std::size_t minFormalCount =
      spec.minFormalCount.value_or(spec.maxFormalCount);
  const auto recordCount =
      5 + (spec.frameDescriptor ? 1 : 0) + spec.frameInputs.size();
  const auto roleTooLong = [&](const std::string_view role) {
    return role.size() > limits.maxMetadataBytes;
  };
  if (roleTooLong(frameReferenceRole) || roleTooLong(captureCountRole) ||
      roleTooLong(minFormalRole) || roleTooLong(maxFormalRole) ||
      roleTooLong(presenceTrueRole) || roleTooLong(presenceFalseRole) ||
      roleTooLong(captureInputRole) || roleTooLong(appliedInputRole) ||
      roleTooLong(contextInputRole) || roleTooLong(presenceInputRole) ||
      zigzag::runtimeObjectProbeText.size() > limits.maxMetadataBytes ||
      closureDescriptorTag.size() > limits.maxMetadataBytes)
    return std::unexpected(
        closureError("closure descriptor metadata exceeds its byte limit"));
  if (spec.frameDescriptor &&
      *spec.frameDescriptor >
          static_cast<CellRef>(std::numeric_limits<std::int64_t>::max()))
    return std::unexpected(closureError(
        "closure frame descriptor reference is not representable"));
  for (std::size_t index = 0; index < recordCount; ++index) arena.consumeWork();
  arena.consumeWork();
  arena.consumeWork();
  try {
    zigzag::ArenaSavepoint savepoint(arena);
    const CellRef trueCell  = arena.makeScalarCell(true);
    const CellRef falseCell = arena.makeScalarCell(false);
    if (trueCell >
            static_cast<CellRef>(std::numeric_limits<std::int64_t>::max()) ||
        falseCell >
            static_cast<CellRef>(std::numeric_limits<std::int64_t>::max()))
      return std::unexpected(
          closureError("closure presence reference is not representable"));
    const CellRef root =
        makeClaimedRoot(core_, closureDescriptorTag, limits.maxMetadataBytes);
    std::optional<CellRef> tail;
    if (spec.frameDescriptor)
      addRecord(core_, root, frameReferenceRole, *spec.frameDescriptor, tail);
    addRecord(core_, root, captureCountRole,
              static_cast<std::uint64_t>(spec.captureStreamCount), tail);
    addRecord(core_, root, minFormalRole,
              static_cast<std::uint64_t>(minFormalCount), tail);
    addRecord(core_, root, maxFormalRole,
              static_cast<std::uint64_t>(spec.maxFormalCount), tail);
    addRecord(core_, root, presenceTrueRole, trueCell, tail);
    addRecord(core_, root, presenceFalseRole, falseCell, tail);
    for (const auto &input : spec.frameInputs) {
      std::string_view role;
      switch (input.source) {
      case ClosureInputSource::Capture:
        role = captureInputRole;
        break;
      case ClosureInputSource::Applied:
        role = appliedInputRole;
        break;
      case ClosureInputSource::Context:
        role = contextInputRole;
        break;
      case ClosureInputSource::AppliedPresence:
        role = presenceInputRole;
        break;
      }
      addRecord(core_, root, role, static_cast<std::uint64_t>(input.index),
                tail);
    }
    savepoint.commit();
    return root;
  } catch (const std::exception &error) {
    return std::unexpected(closureError(error.what()));
  }
} catch (const std::exception &error) {
  return std::unexpected(closureError(error.what()));
}

std::expected<ParsedClosureDescriptor, ClosureDescriptorError>
VortexVM::closureDescriptor(const CellRef root) const try {
  const auto &arena = core_.arena();
  const auto fail   = [](std::string message)
      -> std::expected<ParsedClosureDescriptor, ClosureDescriptorError> {
    return std::unexpected(closureError(std::move(message)));
  };
  const auto tag = claimedTag(core_, root, closureLimits_.maxMetadataBytes,
                              closureLimits_.maxMetadataRelations);
  if (!tag || *tag != closureDescriptorTag)
    return fail("invalid Vortex closure descriptor header or version");

  ParsedClosureDescriptor parsed;
  std::optional<std::size_t> captureCount;
  std::optional<std::size_t> minFormalCount;
  std::optional<std::size_t> maxFormalCount;
  std::optional<CellRef> presenceTrue;
  std::optional<CellRef> presenceFalse;
  std::vector<std::pair<ClosureInputSource, std::size_t>> mappedInputs;
  std::unordered_set<CellRef> visited;
  auto firstRecord =
      boundedMetadataLink(arena, root, core_.dims().grab, DimVector::POS,
                          closureLimits_.maxMetadataRelations);
  if (!firstRecord) return fail(firstRecord.error().message);
  auto record = *firstRecord;
  while (record) {
    arena.consumeWork();
    if (visited.size() >= closureLimits_.maxDescriptorRecords)
      return fail("closure descriptor exceeds the descriptor-record limit");
    if (!visited.insert(*record).second)
      return fail("closure descriptor contains a record cycle");
    if (!arena.contains(*record) ||
        arena.valueKindOf(*record) != xanadu::ValueKind::Int64)
      return fail("closure descriptor record has an invalid value type");
    const auto value = arena.asInt64(*record);
    if (!value || *value < 0)
      return fail("closure descriptor record has an invalid integer");
    const auto role =
        boundedText(arena, *record, closureLimits_.maxMetadataBytes);
    if (!role) return fail("closure descriptor role exceeds its byte limit");
    if (*role == frameReferenceRole) {
      if (parsed.frameDescriptor || static_cast<std::uint64_t>(*value) >
                                        std::numeric_limits<CellRef>::max())
        return fail("closure frame descriptor reference is invalid");
      const CellRef target = static_cast<CellRef>(*value);
      if (!arena.contains(target))
        return fail("closure frame descriptor reference is missing");
      parsed.frameDescriptor = target;
      parsed.referenceCells.emplace_back(*record, target);
    } else if (*role == captureCountRole) {
      if (captureCount || static_cast<std::uint64_t>(*value) > SIZE_MAX)
        return fail("closure capture stream count is duplicated or invalid");
      captureCount = static_cast<std::size_t>(*value);
    } else if (*role == minFormalRole || *role == maxFormalRole) {
      auto &count = *role == minFormalRole ? minFormalCount : maxFormalCount;
      if (count || static_cast<std::uint64_t>(*value) > SIZE_MAX)
        return fail("closure formal count is duplicated or invalid");
      count = static_cast<std::size_t>(*value);
    } else if (*role == presenceTrueRole || *role == presenceFalseRole) {
      auto &reference =
          *role == presenceTrueRole ? presenceTrue : presenceFalse;
      if (reference || static_cast<std::uint64_t>(*value) >
                           std::numeric_limits<CellRef>::max())
        return fail("closure presence reference is duplicated or invalid");
      const CellRef target = static_cast<CellRef>(*value);
      const auto boolean   = arena.asBool(target);
      if (!boolean || *boolean != (*role == presenceTrueRole))
        return fail("closure presence reference has an invalid Bool value");
      reference = target;
      parsed.referenceCells.emplace_back(*record, target);
    } else if (*role == captureInputRole || *role == appliedInputRole ||
               *role == contextInputRole || *role == presenceInputRole) {
      if (static_cast<std::uint64_t>(*value) > SIZE_MAX)
        return fail("closure source index is not representable");
      const auto source =
          *role == captureInputRole   ? ClosureInputSource::Capture
          : *role == appliedInputRole ? ClosureInputSource::Applied
          : *role == contextInputRole ? ClosureInputSource::Context
                                      : ClosureInputSource::AppliedPresence;
      mappedInputs.emplace_back(source, static_cast<std::size_t>(*value));
    } else {
      return fail("closure descriptor contains an unknown record role");
    }
    const auto next =
        boundedMetadataLink(arena, *record, core_.dims().step, DimVector::POS,
                            closureLimits_.maxMetadataRelations);
    if (!next) return fail(next.error().message);
    record = *next;
  }
  if (!captureCount || !minFormalCount || !maxFormalCount || !presenceTrue ||
      !presenceFalse)
    return fail("closure descriptor is missing required counts or references");
  if (*captureCount > closureLimits_.maxCaptureStreams ||
      *maxFormalCount > closureLimits_.maxAppliedStreams ||
      *minFormalCount > *maxFormalCount)
    return fail("closure descriptor exceeds its stream-count limit");
  parsed.captureStreamCount = *captureCount;
  parsed.minFormalCount     = *minFormalCount;
  parsed.maxFormalCount     = *maxFormalCount;
  parsed.presenceTrue       = *presenceTrue;
  parsed.presenceFalse      = *presenceFalse;
  parsed.frameInputs.reserve(mappedInputs.size());
  for (const auto &[source, index] : mappedInputs)
    parsed.frameInputs.push_back({.source = source, .index = index});

  ClosureDescriptorSpec spec{.frameDescriptor    = parsed.frameDescriptor,
                             .captureStreamCount = parsed.captureStreamCount,
                             .minFormalCount     = parsed.minFormalCount,
                             .maxFormalCount     = parsed.maxFormalCount,
                             .frameInputs        = parsed.frameInputs};
  if (const auto valid = validateClosureSpec(*this, spec); !valid)
    return fail(valid.error().message);
  return parsed;
} catch (const std::exception &error) {
  return std::unexpected(closureError(error.what()));
}

std::expected<CellRef, ClosureDescriptorError> VortexVM::makeClosureValue(
    const CellRef descriptor,
    const std::span<const std::vector<CellRef>> resolvedActuals) try {
  auto &arena     = core_.arena();
  const auto fail = [](std::string message)
      -> std::expected<CellRef, ClosureDescriptorError> {
    return std::unexpected(closureError(std::move(message)));
  };
  const auto parsed = closureDescriptor(descriptor);
  if (!parsed) return fail(parsed.error().message);
  if (parsed->captureStreamCount != resolvedActuals.size())
    return fail("closure capture arity does not match its descriptor");
  if (closureValuesCreated_ >= closureLimits_.maxValuesCreated)
    return fail("closure value creation limit exceeded");
  std::size_t capturedRefs = 0;
  for (const auto &actual : resolvedActuals) {
    if (!addWithin(capturedRefs, actual.size(), closureLimits_.maxCapturedRefs))
      return fail("closure captured-reference limit exceeded");
    for (const auto cell : actual) {
      arena.consumeWork();
      if (!arena.contains(cell) ||
          cell > static_cast<CellRef>(std::numeric_limits<std::int64_t>::max()))
        return fail("closure capture refers to a missing cell");
    }
  }
  std::size_t recordCount = 0;
  if (!addWithin(recordCount, 1, closureLimits_.maxValueRecords) ||
      !addWithin(recordCount, parsed->captureStreamCount,
                 closureLimits_.maxValueRecords) ||
      !addWithin(recordCount, capturedRefs, closureLimits_.maxValueRecords))
    return fail("closure value exceeds its record limit");
  std::size_t environmentRecords = 0;
  if (!addWithin(environmentRecords, parsed->captureStreamCount,
                 closureLimits_.maxEnvironmentRecords) ||
      !addWithin(environmentRecords, capturedRefs,
                 closureLimits_.maxEnvironmentRecords))
    return fail("closure environment exceeds its record limit");
  std::size_t valueCells = 0;
  if (!addWithin(valueCells, 3, SIZE_MAX) ||
      !addWithin(valueCells, parsed->captureStreamCount, SIZE_MAX) ||
      !addWithin(valueCells, capturedRefs, SIZE_MAX))
    return fail("closure environment cell count is not representable");
  if (valueCells > closureLimits_.maxEnvironmentCellsCreated -
                       std::min(closureEnvironmentCellsCreated_,
                                closureLimits_.maxEnvironmentCellsCreated))
    return fail("closure environment-cell creation limit exceeded");
  if (valueDescriptorRole.size() > closureLimits_.maxMetadataBytes ||
      environmentStreamRole.size() > closureLimits_.maxMetadataBytes ||
      environmentMemberRole.size() > closureLimits_.maxMetadataBytes ||
      closureValueTag.size() > closureLimits_.maxMetadataBytes)
    return fail("closure value metadata exceeds its byte limit");
  for (std::size_t index = 0; index < recordCount; ++index) arena.consumeWork();
  try {
    zigzag::ArenaSavepoint savepoint(arena);
    const CellRef root = makeClaimedRoot(core_, closureValueTag,
                                         closureLimits_.maxMetadataBytes);
    std::optional<CellRef> tail;
    addRecord(core_, root, valueDescriptorRole, descriptor, tail);
    for (std::size_t index = 0; index < resolvedActuals.size(); ++index) {
      addRecord(core_, root, environmentStreamRole,
                static_cast<std::uint64_t>(index), tail);
      const auto &actual = resolvedActuals[index];
      for (const auto cell : actual)
        addRecord(core_, root, environmentMemberRole, cell, tail);
    }
    savepoint.commit();
    ++closureValuesCreated_;
    closureEnvironmentCellsCreated_ += valueCells;
    return root;
  } catch (const std::exception &error) {
    return fail(error.what());
  }
} catch (const std::exception &error) {
  return std::unexpected(closureError(error.what()));
}

std::expected<ClosureValue, ClosureDescriptorError>
VortexVM::closureValue(const CellRef root) const try {
  const auto &arena = core_.arena();
  const auto fail   = [](std::string message)
      -> std::expected<ClosureValue, ClosureDescriptorError> {
    return std::unexpected(closureError(std::move(message)));
  };
  const auto tag = claimedTag(core_, root, closureLimits_.maxMetadataBytes,
                              closureLimits_.maxMetadataRelations);
  if (!tag || *tag != closureValueTag)
    return fail("invalid Vortex closure value header or version");
  std::unordered_set<CellRef> visited;
  std::optional<CellRef> descriptor;
  std::vector<std::vector<CellRef>> environment;
  std::optional<std::size_t> currentStream;
  std::optional<ParsedClosureDescriptor> parsedClosure;
  std::vector<std::pair<CellRef, CellRef>> references;
  std::size_t environmentRecords = 0;
  std::size_t environmentRefs    = 0;
  auto firstRecord =
      boundedMetadataLink(arena, root, core_.dims().grab, DimVector::POS,
                          closureLimits_.maxMetadataRelations);
  if (!firstRecord) return fail(firstRecord.error().message);
  auto record = *firstRecord;
  while (record) {
    arena.consumeWork();
    if (visited.size() >= closureLimits_.maxValueRecords)
      return fail("closure value exceeds its record limit");
    if (!visited.insert(*record).second)
      return fail("closure value contains a record cycle");
    if (!arena.contains(*record) ||
        arena.valueKindOf(*record) != xanadu::ValueKind::Int64)
      return fail("closure value record has an invalid value type");
    const auto value = arena.asInt64(*record);
    if (!value || *value < 0)
      return fail("closure value record has an invalid integer");
    const auto role =
        boundedText(arena, *record, closureLimits_.maxMetadataBytes);
    if (!role) return fail("closure value role exceeds its byte limit");
    if (*role == valueDescriptorRole) {
      if (descriptor) return fail("closure value has duplicate descriptors");
      if (static_cast<std::uint64_t>(*value) >
          std::numeric_limits<CellRef>::max())
        return fail("closure descriptor reference is not representable");
      const CellRef target = static_cast<CellRef>(*value);
      if (!arena.contains(target))
        return fail("closure value descriptor is missing");
      const auto parsedDescriptor = closureDescriptor(target);
      if (!parsedDescriptor)
        return fail("closure value descriptor: " +
                    parsedDescriptor.error().message);
      parsedClosure = *parsedDescriptor;
      descriptor    = target;
      references.emplace_back(*record, target);
      for (const auto &reference : parsedDescriptor->referenceCells)
        references.push_back(reference);
    } else if (*role == environmentStreamRole) {
      if (!descriptor)
        return fail("closure environment precedes its descriptor");
      if (static_cast<std::uint64_t>(*value) > SIZE_MAX ||
          static_cast<std::size_t>(*value) != environment.size())
        return fail("closure environment stream order is invalid");
      if (!parsedClosure ||
          environment.size() >= parsedClosure->captureStreamCount)
        return fail("closure environment has too many streams");
      if (!addWithin(environmentRecords, 1,
                     closureLimits_.maxEnvironmentRecords))
        return fail("closure environment exceeds its record limit");
      environment.emplace_back();
      currentStream = environment.size() - 1;
    } else if (*role == environmentMemberRole) {
      if (!currentStream)
        return fail("closure environment member has no stream");
      if (static_cast<std::uint64_t>(*value) >
          std::numeric_limits<CellRef>::max())
        return fail("closure environment CellRef is not representable");
      const CellRef target = static_cast<CellRef>(*value);
      if (!arena.contains(target))
        return fail("closure environment refers to a missing cell");
      if (!addWithin(environmentRecords, 1,
                     closureLimits_.maxEnvironmentRecords) ||
          !addWithin(environmentRefs, 1, closureLimits_.maxCapturedRefs))
        return fail("closure environment exceeds its reference limit");
      if (!currentStream ||
          environment[*currentStream].size() >= closureLimits_.maxCapturedRefs)
        return fail("closure environment exceeds its reference limit");
      environment[*currentStream].push_back(target);
      references.emplace_back(*record, target);
    } else {
      return fail("closure value contains an unknown record role");
    }
    const auto next =
        boundedMetadataLink(arena, *record, core_.dims().step, DimVector::POS,
                            closureLimits_.maxMetadataRelations);
    if (!next) return fail(next.error().message);
    record = *next;
  }
  if (!descriptor) return fail("closure value has no descriptor reference");
  if (!parsedClosure)
    return fail("closure value descriptor could not be parsed");
  if (environment.size() != parsedClosure->captureStreamCount)
    return fail("closure environment stream arity is invalid");
  ClosureValue result{.descriptor     = descriptor,
                      .captures       = std::move(environment),
                      .referenceCells = std::move(references)};
  return result;
} catch (const std::exception &error) {
  return std::unexpected(closureError(error.what()));
}

ExecutionResult VortexVM::executeClosureOpcode(const OpcodeKind kind,
                                               const CellRef opcode,
                                               const CellRef cursor) {
  return executeClosureOpcodeBody(kind, opcode, cursor);
}

ExecutionResult VortexVM::executeClosureOpcodeBody(const OpcodeKind kind,
                                                   const CellRef opcode,
                                                   const CellRef cursor) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };
  auto &arena                  = core_.arena();
  const std::size_t inputLimit = kind == OpcodeKind::CreateClosure
                                     ? 1 + closureLimits_.maxCaptureStreams
                                 : kind == OpcodeKind::CheckClosureCall
                                     ? 3
                                     : 3 + closureLimits_.maxAppliedStreams;
  const auto rawInputWing =
      closureWing(core_, opcode, DimVector::POS, inputLimit,
                  closureLimits_.maxMetadataRelations);
  if (!rawInputWing) return fail(rawInputWing.error().message);
  const auto rawOutputWing = closureWing(
      core_, opcode, DimVector::NEG,
      kind == OpcodeKind::CreateClosure || kind == OpcodeKind::CheckClosureCall
          ? 1
          : frameLimits_.maxDescriptorRecords,
      closureLimits_.maxMetadataRelations);
  if (!rawOutputWing) return fail(rawOutputWing.error().message);
  const auto &rawInputs = *rawInputWing;
  auto inputs           = rawInputs;
  auto outputs          = *rawOutputWing;
  for (auto &input : inputs) input = runtimeSlot(input);
  for (auto &output : outputs) output = runtimeSlot(output);
  const auto continuation =
      arena.linked(opcode, core_.dims().spin, DimVector::POS);
  if (isMemoized(opcode) ||
      arena.linked(opcode, core_.dims().contract, DimVector::POS) ||
      arena.linked(opcode, core_.dims().contract, DimVector::NEG))
    return fail("closure opcodes do not support conditions or memoization");
  for (const auto slot : rawInputs)
    if (closureOperandHasPipeline(core_, slot))
      return fail("closure opcodes do not support parameter pipelines");
  for (const auto slot : *rawOutputWing)
    if (closureOperandHasPipeline(core_, slot))
      return fail("closure opcodes do not support parameter pipelines");

  if (kind == OpcodeKind::CreateClosure) {
    if (inputs.empty() || outputs.size() != 1)
      return fail("CreateClosure requires a descriptor, capture sources, and "
                  "one output");
    if (cellReferences_.contains(rawInputs.front()) ||
        !arena.contains(rawInputs.front()))
      return fail("CreateClosure descriptor must be a direct graph reference");
    const auto descriptor = closureDescriptor(rawInputs.front());
    if (!descriptor) return fail(descriptor.error().message);
    if (inputs.size() - 1 != descriptor->captureStreamCount)
      return fail("CreateClosure capture arity does not match its descriptor");
    const CellRef output = outputs.front();
    if (!arena.contains(output) || getOpcodeKind(output) ||
        streamSlots_.contains(output) || iteratorSlots_.contains(output) ||
        output == rawInputs.front() ||
        std::ranges::any_of(
            inputs, [output](CellRef input) { return input == output; }))
      return fail("CreateClosure output must be a distinct scalar caller slot");

    std::array<std::size_t, closureStreamCapacity> captureCounts{};
    std::size_t captureCountIndex = 0;
    std::size_t capturedCount     = 0;
    for (std::size_t index = 1; index < inputs.size(); ++index) {
      const CellRef source = inputs[index];
      if (iteratorSlots_.contains(source))
        return fail("CreateClosure iterator captures are unsupported");
      std::size_t sourceCount = 0;
      forEachResultCell(source, [&](const CellRef cell) {
        arena.consumeWork();
        if (!arena.contains(cell) ||
            !addWithin(sourceCount, 1, closureLimits_.maxCapturedRefs) ||
            !addWithin(capturedCount, 1, closureLimits_.maxCapturedRefs))
          throw std::length_error("closure captured-reference limit exceeded");
      });
      captureCounts[captureCountIndex++] = sourceCount;
    }
    std::vector<std::vector<CellRef>> captures;
    captures.reserve(descriptor->captureStreamCount);
    for (std::size_t index = 1; index < inputs.size(); ++index) {
      auto &capture = captures.emplace_back();
      capture.reserve(captureCounts[index - 1]);
      forEachResultCell(inputs[index], [&](const CellRef cell) {
        arena.consumeWork();
        capture.push_back(cell);
      });
    }
    auto priorReferences                    = cellReferences_;
    auto priorCursorPC                      = cursorPC_;
    const auto priorValuesCreated           = closureValuesCreated_;
    const auto priorEnvironmentCellsCreated = closureEnvironmentCellsCreated_;
    try {
      zigzag::ArenaSavepoint savepoint(arena);
      const auto value = makeClosureValue(rawInputs.front(), captures);
      if (!value) return fail(value.error().message);
      writeCellReferences(outputs,
                          {CellValue{static_cast<std::int64_t>(*value)}});
      setCursorOpcode(cursor, continuation);
      savepoint.commit();
      return {};
    } catch (const std::exception &error) {
      cellReferences_.swap(priorReferences);
      cursorPC_.swap(priorCursorPC);
      closureValuesCreated_           = priorValuesCreated;
      closureEnvironmentCellsCreated_ = priorEnvironmentCellsCreated;
      return fail(error.what());
    }
  }

  if (kind == OpcodeKind::CheckClosureCall) {
    if (inputs.size() != 3 || outputs.size() != 1)
      return fail(
          "CheckClosureCall requires callee, mode, count and one output");
    const CellRef calleeSlot = inputs[0];
    const CellRef modeSlot   = inputs[1];
    const CellRef countSlot  = inputs[2];
    const CellRef output     = outputs.front();
    if (!arena.contains(output) || getOpcodeKind(output) ||
        streamSlots_.contains(output) || iteratorSlots_.contains(output) ||
        output == calleeSlot || output == modeSlot || output == countSlot ||
        streamSlots_.contains(modeSlot) || iteratorSlots_.contains(modeSlot) ||
        streamSlots_.contains(countSlot) || iteratorSlots_.contains(countSlot))
      return fail(
          "CheckClosureCall requires scalar operands and a distinct output");
    const auto rawMode  = arena.asInt64(modeSlot);
    const auto rawCount = arena.asInt64(countSlot);
    if (!rawMode ||
        (*rawMode != static_cast<std::int64_t>(ClosureCallMode::Explicit) &&
         *rawMode != static_cast<std::int64_t>(ClosureCallMode::Contextual)))
      return fail("CheckClosureCall has an invalid call mode");
    if (!rawCount || *rawCount < 0 ||
        static_cast<std::uint64_t>(*rawCount) >
            closureLimits_.maxAppliedStreams)
      return fail("CheckClosureCall has an invalid explicit argument count");
    const auto mode = static_cast<ClosureCallMode>(*rawMode);
    if (mode == ClosureCallMode::Contextual && *rawCount != 0)
      return fail("Contextual CheckClosureCall cannot have explicit arguments");

    std::optional<CellRef> handle;
    std::size_t handleCount = 0;
    try {
      forEachResultCell(calleeSlot, [&](const CellRef cell) {
        arena.consumeWork();
        if (handleCount != 0)
          throw std::length_error(
              "CheckClosureCall requires exactly one closure handle");
        if (!arena.contains(cell))
          throw std::invalid_argument(
              "CheckClosureCall handle does not resolve to a cell");
        handle = cell;
        ++handleCount;
      });
    } catch (const std::exception &error) {
      return fail(error.what());
    }
    if (!handle)
      return fail("CheckClosureCall requires exactly one closure handle");
    if (*handle >
        static_cast<CellRef>(std::numeric_limits<std::int64_t>::max()))
      return fail("CheckClosureCall handle reference is not representable");
    const auto value = closureValue(*handle);
    if (!value) return fail(value.error().message);
    if (!value->descriptor)
      return fail("CheckClosureCall closure has no descriptor");
    const auto descriptor = closureDescriptor(*value->descriptor);
    if (!descriptor) return fail(descriptor.error().message);
    if (!descriptor->frameDescriptor)
      return fail("CheckClosureCall descriptor has no frame");
    const auto frame = frameDescriptor(*descriptor->frameDescriptor);
    if (!frame) return fail(frame.error().message);
    const std::size_t rawArguments = static_cast<std::size_t>(*rawCount);
    const std::size_t effectiveArguments =
        mode == ClosureCallMode::Contextual
            ? static_cast<std::size_t>(descriptor->maxFormalCount != 0)
            : rawArguments;
    if (effectiveArguments < descriptor->minFormalCount ||
        effectiveArguments > descriptor->maxFormalCount)
      return fail(
          "CheckClosureCall argument count is outside the closure arity");

    std::size_t snapshotRecords = 0;
    if (!addWithin(snapshotRecords, cellReferences_.size(),
                   closureLimits_.maxInvocationRefs) ||
        !addWithin(snapshotRecords, cursorPC_.size(),
                   closureLimits_.maxInvocationRefs))
      return fail("CheckClosureCall snapshot limit exceeded");
    for (std::size_t index = 0; index < snapshotRecords; ++index)
      arena.consumeWork();
    auto priorReferences = cellReferences_;
    auto priorCursorPC   = cursorPC_;
    try {
      zigzag::ArenaSavepoint savepoint(arena);
      writeCellReferences(outputs,
                          {CellValue{static_cast<std::int64_t>(*handle)}});
      setCursorOpcode(cursor, continuation);
      savepoint.commit();
      return {};
    } catch (const std::exception &error) {
      cellReferences_.swap(priorReferences);
      cursorPC_.swap(priorCursorPC);
      return fail(error.what());
    }
  }

  if (inputs.size() < 3)
    return fail("ApplyClosure requires handle, mode and hidden context");
  const CellRef modeSlot = inputs[1];
  if (streamSlots_.contains(modeSlot) || iteratorSlots_.contains(modeSlot))
    return fail("ApplyClosure mode must be an Int64 cell");
  const auto rawMode = arena.asInt64(modeSlot);
  if (!rawMode ||
      (*rawMode != static_cast<std::int64_t>(ClosureCallMode::Explicit) &&
       *rawMode != static_cast<std::int64_t>(ClosureCallMode::Contextual)))
    return fail("ApplyClosure has an invalid call mode");
  const auto mode                    = static_cast<ClosureCallMode>(*rawMode);
  const std::size_t rawArgumentCount = inputs.size() - 3;
  if (mode == ClosureCallMode::Contextual && rawArgumentCount != 0)
    return fail("Contextual ApplyClosure cannot have explicit arguments");

  std::optional<CellRef> handle;
  std::size_t handleCount = 0;
  try {
    forEachResultCell(inputs.front(), [&](const CellRef cell) {
      arena.consumeWork();
      if (handleCount != 0)
        throw std::length_error(
            "ApplyClosure requires exactly one closure handle; input has "
            "multiple values");
      if (!arena.contains(cell))
        throw std::invalid_argument(
            "ApplyClosure handle does not resolve to a cell");
      handle = cell;
      ++handleCount;
    });
  } catch (const std::exception &error) {
    return fail(error.what());
  }
  if (!handle)
    return fail("ApplyClosure requires exactly one closure handle; input is "
                "empty");
  const auto value = closureValue(*handle);
  if (!value) return fail(value.error().message);
  if (!value->descriptor) return fail("ApplyClosure closure has no descriptor");
  const auto descriptor = closureDescriptor(*value->descriptor);
  if (!descriptor) return fail(descriptor.error().message);
  const std::size_t effectiveArgumentCount =
      mode == ClosureCallMode::Contextual
          ? static_cast<std::size_t>(descriptor->maxFormalCount != 0)
          : rawArgumentCount;
  if (rawArgumentCount > closureLimits_.maxAppliedStreams ||
      effectiveArgumentCount < descriptor->minFormalCount ||
      effectiveArgumentCount > descriptor->maxFormalCount)
    return fail("ApplyClosure argument count is outside the closure arity");
  if (!descriptor->frameDescriptor)
    return fail("ApplyClosure descriptor has no frame");
  const auto frame = frameDescriptor(*descriptor->frameDescriptor);
  if (!frame) return fail(frame.error().message);
  if (outputs.size() != frame->resultPorts.size())
    return fail("ApplyClosure output arity does not match frame results");

  const CellRef contextSlot = inputs[2];
  if (iteratorSlots_.contains(contextSlot))
    return fail("ApplyClosure hidden context iterator is unsupported");
  std::array<std::size_t, closureStreamCapacity> sourceCounts{};
  std::size_t contextCount = 0;
  std::size_t sourceRefs   = 0;
  const auto countStream   = [&](const CellRef source, std::size_t &count) {
    forEachResultCell(source, [&](const CellRef cell) {
      arena.consumeWork();
      if (!arena.contains(cell) ||
          !addWithin(count, 1, closureLimits_.maxInvocationRefs))
        throw std::length_error(
            "ApplyClosure invocation-reference limit exceeded");
    });
  };
  try {
    countStream(contextSlot, contextCount);
    if (!addWithin(sourceRefs, contextCount, closureLimits_.maxInvocationRefs))
      return fail("ApplyClosure invocation-reference limit exceeded");
    for (std::size_t index = 0; index < rawArgumentCount; ++index) {
      const CellRef source = inputs[index + 3];
      if (iteratorSlots_.contains(source))
        return fail("ApplyClosure iterator arguments are unsupported");
      countStream(source, sourceCounts[index]);
      if (!addWithin(sourceRefs, sourceCounts[index],
                     closureLimits_.maxInvocationRefs))
        return fail("ApplyClosure invocation-reference limit exceeded");
    }
  } catch (const std::exception &error) {
    return fail(error.what());
  }
  if (mode == ClosureCallMode::Contextual && descriptor->maxFormalCount != 0)
    sourceCounts[0] = contextCount;

  std::size_t capturedRefs = 0;
  for (const auto &source : value->captures)
    if (!addWithin(capturedRefs, source.size(),
                   closureLimits_.maxInvocationRefs))
      return fail("ApplyClosure invocation-reference limit exceeded");
  if (!addWithin(sourceRefs, capturedRefs, closureLimits_.maxInvocationRefs))
    return fail("ApplyClosure invocation-reference limit exceeded");

  std::size_t fanoutRefs = 0;
  for (std::size_t index = 0; index < descriptor->frameInputs.size(); ++index) {
    const auto &mapping = descriptor->frameInputs[index];
    std::size_t count   = 0;
    switch (mapping.source) {
    case ClosureInputSource::Capture:
      count = value->captures.at(mapping.index).size();
      break;
    case ClosureInputSource::Applied:
      count = sourceCounts[mapping.index];
      break;
    case ClosureInputSource::Context:
      count = contextCount;
      break;
    case ClosureInputSource::AppliedPresence:
      count = 1;
      break;
    }
    if (frame->argumentPorts[index].kind == FramePortKind::Cell && count > 1)
      return fail("ApplyClosure cell frame source contains multiple cells");
    if (!addWithin(fanoutRefs, count, closureLimits_.maxInvocationRefs))
      return fail("ApplyClosure mapped-reference limit exceeded");
  }
  std::size_t retainedAndMappedRefs = sourceRefs;
  if (!addWithin(retainedAndMappedRefs, fanoutRefs,
                 closureLimits_.maxInvocationRefs))
    return fail("ApplyClosure aggregate-reference limit exceeded");
  for (std::size_t index = 0; index < fanoutRefs; ++index) arena.consumeWork();

  std::vector<CellRef> context;
  context.reserve(contextCount);
  forEachResultCell(contextSlot, [&](const CellRef cell) {
    arena.consumeWork();
    context.push_back(cell);
  });
  std::vector<std::vector<CellRef>> applied(descriptor->maxFormalCount);
  for (std::size_t index = 0; index < rawArgumentCount; ++index) {
    auto &argument = applied[index];
    argument.reserve(sourceCounts[index]);
    forEachResultCell(inputs[index + 3], [&](const CellRef cell) {
      arena.consumeWork();
      argument.push_back(cell);
    });
  }
  if (mode == ClosureCallMode::Contextual && descriptor->maxFormalCount != 0)
    applied[0] = context;

  std::vector<std::vector<CellRef>> arguments;
  arguments.reserve(descriptor->frameInputs.size());
  for (const auto &mapping : descriptor->frameInputs) {
    switch (mapping.source) {
    case ClosureInputSource::Capture:
      arguments.push_back(value->captures.at(mapping.index));
      break;
    case ClosureInputSource::Applied:
      arguments.push_back(applied.at(mapping.index));
      break;
    case ClosureInputSource::Context:
      arguments.push_back(context);
      break;
    case ClosureInputSource::AppliedPresence:
      arguments.push_back({mapping.index < effectiveArgumentCount
                               ? *descriptor->presenceTrue
                               : *descriptor->presenceFalse});
      break;
    }
  }
  return invokeFramePrepared(*descriptor->frameDescriptor, arguments, outputs,
                             cursor, continuation, opcode);
}

} // namespace zigzag::vortex

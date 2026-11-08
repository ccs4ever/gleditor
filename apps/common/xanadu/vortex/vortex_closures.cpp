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

constexpr std::string_view closureDescriptorTag = "VORTEX_CLOSURE_DESCRIPTOR/1";
constexpr std::string_view closureValueTag      = "VORTEX_CLOSURE_VALUE/1";
constexpr std::string_view frameReferenceRole   = "ref.frame-descriptor";
constexpr std::string_view captureCountRole     = "count.capture-streams";
constexpr std::string_view appliedCountRole     = "count.applied-streams";
constexpr std::string_view captureInputRole     = "input.capture";
constexpr std::string_view appliedInputRole     = "input.applied";
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
  if (spec.captureStreamCount > limits.maxCaptureStreams ||
      spec.appliedStreamCount > limits.maxAppliedStreams)
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
  if (frame &&
      std::ranges::any_of(frame->argumentPorts, [](const FramePort &port) {
        return port.kind != FramePortKind::Stream;
      }))
    return fail("closure frame arguments must all be Stream ports");
  const std::size_t fixedRecords = 2 + (spec.frameDescriptor ? 1 : 0);
  if (limits.maxDescriptorRecords < fixedRecords ||
      spec.frameInputs.size() > limits.maxDescriptorRecords - fixedRecords)
    return fail("closure descriptor exceeds the descriptor-record limit");

  std::vector<bool> captureUsed(spec.captureStreamCount, false);
  std::vector<bool> appliedUsed(spec.appliedStreamCount, false);
  for (const auto &input : spec.frameInputs) {
    auto *used =
        input.source == ClosureInputSource::Capture
            ? (input.index < captureUsed.size() ? &captureUsed : nullptr)
        : input.source == ClosureInputSource::Applied
            ? (input.index < appliedUsed.size() ? &appliedUsed : nullptr)
            : nullptr;
    if (used == nullptr)
      return fail("closure source map contains an out-of-range source");
    (*used)[input.index] = true;
  }
  if (std::ranges::any_of(captureUsed, [](bool used) { return !used; }) ||
      std::ranges::any_of(appliedUsed, [](bool used) { return !used; }))
    return fail("every closure source stream must be used by the frame");
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
  const auto recordCount =
      2 + (spec.frameDescriptor ? 1 : 0) + spec.frameInputs.size();
  const auto roleTooLong = [&](const std::string_view role) {
    return role.size() > limits.maxMetadataBytes;
  };
  if (roleTooLong(frameReferenceRole) || roleTooLong(captureCountRole) ||
      roleTooLong(appliedCountRole) || roleTooLong(captureInputRole) ||
      roleTooLong(appliedInputRole) ||
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
  try {
    zigzag::ArenaSavepoint savepoint(arena);
    const CellRef root =
        makeClaimedRoot(core_, closureDescriptorTag, limits.maxMetadataBytes);
    std::optional<CellRef> tail;
    if (spec.frameDescriptor)
      addRecord(core_, root, frameReferenceRole, *spec.frameDescriptor, tail);
    addRecord(core_, root, captureCountRole,
              static_cast<std::uint64_t>(spec.captureStreamCount), tail);
    addRecord(core_, root, appliedCountRole,
              static_cast<std::uint64_t>(spec.appliedStreamCount), tail);
    for (const auto &input : spec.frameInputs)
      addRecord(core_, root,
                input.source == ClosureInputSource::Capture ? captureInputRole
                                                            : appliedInputRole,
                static_cast<std::uint64_t>(input.index), tail);
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
  std::optional<std::size_t> appliedCount;
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
    } else if (*role == appliedCountRole) {
      if (appliedCount || static_cast<std::uint64_t>(*value) > SIZE_MAX)
        return fail("closure applied stream count is duplicated or invalid");
      appliedCount = static_cast<std::size_t>(*value);
    } else if (*role == captureInputRole || *role == appliedInputRole) {
      if (static_cast<std::uint64_t>(*value) > SIZE_MAX)
        return fail("closure source index is not representable");
      mappedInputs.emplace_back(*role == captureInputRole
                                    ? ClosureInputSource::Capture
                                    : ClosureInputSource::Applied,
                                static_cast<std::size_t>(*value));
    } else {
      return fail("closure descriptor contains an unknown record role");
    }
    const auto next =
        boundedMetadataLink(arena, *record, core_.dims().step, DimVector::POS,
                            closureLimits_.maxMetadataRelations);
    if (!next) return fail(next.error().message);
    record = *next;
  }
  if (!captureCount || !appliedCount)
    return fail("closure descriptor is missing stream counts");
  if (*captureCount > closureLimits_.maxCaptureStreams ||
      *appliedCount > closureLimits_.maxAppliedStreams)
    return fail("closure descriptor exceeds its stream-count limit");
  parsed.captureStreamCount = *captureCount;
  parsed.appliedStreamCount = *appliedCount;
  parsed.frameInputs.reserve(mappedInputs.size());
  for (const auto &[source, index] : mappedInputs)
    parsed.frameInputs.push_back({.source = source, .index = index});

  ClosureDescriptorSpec spec{.frameDescriptor    = parsed.frameDescriptor,
                             .captureStreamCount = parsed.captureStreamCount,
                             .appliedStreamCount = parsed.appliedStreamCount,
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
  auto &arena = core_.arena();
  const auto rawInputWing =
      closureWing(core_, opcode, DimVector::POS,
                  1 + (kind == OpcodeKind::CreateClosure
                           ? closureLimits_.maxCaptureStreams
                           : closureLimits_.maxAppliedStreams),
                  closureLimits_.maxMetadataRelations);
  if (!rawInputWing) return fail(rawInputWing.error().message);
  const auto rawOutputWing = closureWing(
      core_, opcode, DimVector::NEG,
      kind == OpcodeKind::CreateClosure ? 1 : frameLimits_.maxDescriptorRecords,
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

  if (inputs.empty()) return fail("ApplyClosure requires a closure handle");
  const auto handle = resultCell(cursor, inputs.front());
  if (!handle || !arena.contains(*handle))
    return fail("ApplyClosure handle does not resolve to a cell");
  const auto value = closureValue(*handle);
  if (!value) return fail(value.error().message);
  const auto descriptor = closureDescriptor(*value->descriptor);
  if (!descriptor) return fail(descriptor.error().message);
  if (inputs.size() - 1 != descriptor->appliedStreamCount)
    return fail("ApplyClosure applied arity does not match its descriptor");
  if (!descriptor->frameDescriptor)
    return fail("ApplyClosure descriptor has no frame");
  const auto frame = frameDescriptor(*descriptor->frameDescriptor);
  if (!frame) return fail(frame.error().message);
  if (outputs.size() != frame->resultPorts.size())
    return fail("ApplyClosure output arity does not match frame results");

  std::vector<std::vector<CellRef>> applied;
  std::size_t capturedRefs = 0;
  for (const auto &source : value->captures)
    if (!addWithin(capturedRefs, source.size(),
                   closureLimits_.maxInvocationRefs))
      return fail("ApplyClosure invocation-reference limit exceeded");
  std::size_t sourceRefs = 0;
  std::array<std::size_t, closureStreamCapacity> sourceCounts{};
  std::size_t sourceCountIndex = 0;
  for (std::size_t index = 1; index < inputs.size(); ++index) {
    const CellRef source = inputs[index];
    if (iteratorSlots_.contains(source))
      return fail("ApplyClosure iterator arguments are unsupported");
    std::size_t count = 0;
    forEachResultCell(source, [&](const CellRef cell) {
      arena.consumeWork();
      if (!arena.contains(cell) ||
          !addWithin(count, 1, closureLimits_.maxInvocationRefs))
        throw std::length_error(
            "ApplyClosure invocation-reference limit exceeded");
    });
    if (!addWithin(sourceRefs, count, closureLimits_.maxInvocationRefs))
      return fail("ApplyClosure invocation-reference limit exceeded");
    sourceCounts[sourceCountIndex++] = count;
  }
  if (!addWithin(sourceRefs, capturedRefs, closureLimits_.maxInvocationRefs))
    return fail("ApplyClosure invocation-reference limit exceeded");

  std::size_t fanoutRefs = 0;
  for (const auto &mapping : descriptor->frameInputs) {
    const std::size_t count = mapping.source == ClosureInputSource::Capture
                                  ? value->captures.at(mapping.index).size()
                                  : sourceCounts[mapping.index];
    if (!addWithin(fanoutRefs, count, closureLimits_.maxInvocationRefs))
      return fail("ApplyClosure mapped-reference limit exceeded");
  }
  std::size_t retainedAndMappedRefs = sourceRefs;
  if (!addWithin(retainedAndMappedRefs, fanoutRefs,
                 closureLimits_.maxInvocationRefs))
    return fail("ApplyClosure aggregate-reference limit exceeded");
  for (std::size_t index = 0; index < fanoutRefs; ++index) arena.consumeWork();

  applied.reserve(descriptor->appliedStreamCount);
  for (std::size_t index = 1; index < inputs.size(); ++index) {
    auto &source = applied.emplace_back();
    source.reserve(sourceCounts[index - 1]);
    forEachResultCell(inputs[index], [&](const CellRef cell) {
      arena.consumeWork();
      source.push_back(cell);
    });
  }

  std::vector<std::vector<CellRef>> arguments;
  arguments.reserve(descriptor->frameInputs.size());
  for (const auto &mapping : descriptor->frameInputs) {
    const auto &source = mapping.source == ClosureInputSource::Capture
                             ? value->captures.at(mapping.index)
                             : applied.at(mapping.index);
    arguments.push_back(source);
  }
  return invokeFramePrepared(*descriptor->frameDescriptor, arguments, outputs,
                             cursor, continuation, opcode);
}

} // namespace zigzag::vortex

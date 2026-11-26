#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/file_endpoint_adapter.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/io_service.hpp"

namespace {
using namespace xanadu::vortex;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;

std::vector<std::byte> bytesOf(std::string_view text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i)
    out[i] = static_cast<std::byte>(text[i]);
  return out;
}

std::string textOf(const std::vector<std::byte> &bytes) {
  std::string out(bytes.size(), '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i)
    out[i] = static_cast<char>(bytes[i]);
  return out;
}

/// The arena the file endpoints' cells live in is also the arena the spec
/// cells are minted in: the RequestDecoder reads spec text through it and
/// stageAt keys are the minted cells' refs.
struct FileEndpointFixture {
  ArenaManifold arena;
  IoCellSchema schema{makeIoCellSchema(arena).value()};
  PayloadResolver payloads;
  FileEndpointAdapter adapter;
  ArenaIoCellStore cells;
  std::unique_ptr<BoundedIoScheduler> scheduler;
  std::unique_ptr<IoService> service;

  explicit FileEndpointFixture(std::size_t payloadBound = 1U << 20)
      : cells(arena, schema, payloads,
              makeFileRequestDecoder(arena, fileDataAdapterKey, payloadBound),
              makeFileCloseDecoder(adapter),
              [](const OpenSpec &)
                  -> std::expected<ArenaIoCellStore::OpenGrant, IoErrorInfo> {
                return ArenaIoCellStore::OpenGrant{
                    .rights      = fileRightRead | fileRightsMutating,
                    .consistency = ioConsistencyLive};
              }) {
    scheduler = std::make_unique<BoundedIoScheduler>(
        BoundedIoSchedulerOptions{2, 8, 1 << 22, 1 << 22}, payloads);
    service = std::make_unique<IoService>(
        IoBudgetSnapshot{16, 1 << 22, 1 << 22, 1 << 22, 8},
        std::move(scheduler), payloads, cells);
    service->registerAdapter(fileOpenAdapterKey, adapter);
    service->registerAdapter(fileDataAdapterKey, adapter);
    service->registerAdapter(fileCloseAdapterKey, adapter);
  }
  FileEndpointFixture(const FileEndpointFixture &)                     = delete;
  auto operator=(const FileEndpointFixture &) -> FileEndpointFixture & = delete;
  ~FileEndpointFixture() {
    if (service)
      static_cast<void>(service->requestShutdown(ShutdownMode::CancelPending));
  }
};

/// Pump the service until the request's terminal outcome materializes.
std::optional<OutcomeCell> awaitOutcome(IoService &service,
                                        const RequestCell request) {
  for (int i = 0; i < 400; ++i) {
    if (const auto poll = service.poll(request); poll.outcome.has_value()) {
      return *poll.outcome;
    }
    static_cast<void>(service.pump(8));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return std::nullopt;
}

/// The endpoint/capability an open outcome minted, read back through the
/// outcome cell's d.io.endpoint/d.io.capability canonical fields -- the
/// same walk capabilityEndpoint performs for a request's fields.
std::optional<OpenedEndpoint> openedEndpointOf(const IoCellSchema &schema,
                                               const ArenaManifold &arena,
                                               const OutcomeCell outcome) {
  const auto field = [&](const IoDimension which) -> std::optional<CellRef> {
    const auto dim = schema.dim(which);
    const auto occ = arena.linked(outcome.cell, dim, DimVector::POS);
    if (!occ) return std::nullopt;
    const auto view = arena.linked(*occ, dim, DimVector::POS);
    if (!view) return std::nullopt;
    const auto canonical =
        arena.cloneMaster(*view, schema.dim(IoDimension::Clone));
    if (!canonical || *canonical == *view) return std::nullopt;
    return *canonical;
  };
  const auto endpoint   = field(IoDimension::Endpoint);
  const auto capability = field(IoDimension::Capability);
  if (!endpoint || !capability) return std::nullopt;
  const auto endpointInc   = arena.incarnationOf(*endpoint);
  const auto capabilityInc = arena.incarnationOf(*capability);
  if (!endpointInc || !capabilityInc) return std::nullopt;
  return OpenedEndpoint{EndpointCell{*endpoint, *endpointInc},
                        CapabilityCell{*capability, *capabilityInc}};
}

/// The open the adapter bound to @p opened's endpoint.
FileOpenId openOf(const FileEndpointFixture &fixture,
                  const std::optional<OpenedEndpoint> &opened) {
  EXPECT_TRUE(opened.has_value());
  if (!opened) return FileOpenId{};
  const auto id = fixture.adapter.openIdOf(opened->endpoint);
  EXPECT_TRUE(id.has_value());
  return id.value_or(FileOpenId{});
}

/// Mint a spec cell whose text tags the op for makeFileRequestDecoder and
/// stage the matching FileEndpointOp under the cell's own ref.
CellRef stageSpec(FileEndpointFixture &fixture, std::string_view tag,
                  const FileOpenId open, FileOpSpec op) {
  const auto spec   = fixture.arena.makeCell(tag);
  const auto staged = fixture.adapter.stageAt(
      spec, FileEndpointOp{.open = open, .op = std::move(op)});
  EXPECT_TRUE(staged);
  return spec;
}

/// Submit @p spec through the service and return the terminal outcome cell.
std::optional<OutcomeCell> submitAndAwait(FileEndpointFixture &fixture,
                                          const CapabilityCell capability,
                                          const CellRef spec) {
  const auto submitted = fixture.service->submit(capability, spec);
  if (!std::holds_alternative<Admitted>(submitted.value)) {
    return std::get<Refused>(submitted.value).outcome;
  }
  return awaitOutcome(*fixture.service,
                      std::get<Admitted>(submitted.value).request);
}

/// admitOpen @p path and return the minted endpoint/capability once its
/// terminal outcome publishes.
std::optional<OpenedEndpoint> openAndAwait(FileEndpointFixture &fixture,
                                           const std::string &path,
                                           std::uint64_t rights,
                                           std::uint64_t consistency) {
  OpenSpec spec{.adapterKey      = fileOpenAdapterKey,
                .locator         = bytesOf(path),
                .requestedRights = rights,
                .consistency     = consistency,
                .deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5)};
  const auto submitted = fixture.service->admitOpen(spec);
  if (!std::holds_alternative<Admitted>(submitted.value)) {
    return std::nullopt;
  }
  const auto outcome = awaitOutcome(
      *fixture.service, std::get<Admitted>(submitted.value).request);
  if (!outcome) return std::nullopt;
  return openedEndpointOf(fixture.schema, fixture.arena, *outcome);
}

/// Read the retained bytes an outcome's payload range names.
std::expected<std::vector<std::byte>, IoError>
readOutcomePayload(FileEndpointFixture &fixture, const OutcomeCell outcome) {
  const auto range =
      outcomeCellPayloadRange(fixture.arena, fixture.schema, outcome);
  if (!range) return std::unexpected(IoError{IoErrorCode::Unresolved, 0});
  auto lease = fixture.payloads.retain(*range);
  if (!lease) return std::unexpected(lease.error());
  auto bytes = fixture.payloads.read(
      *lease, 0, static_cast<std::size_t>(lease->range.length));
  fixture.payloads.release(lease->lease);
  return bytes;
}

class FileEndpointAdapterTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto base = std::filesystem::temp_directory_path() /
                      ("io_file_endpoint_test_" + std::to_string(::getpid()) +
                       "_" + std::to_string(seq_++));
    std::filesystem::create_directories(base);
    dir_ = base;
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::string makeFile(std::string_view content) const {
    const auto path = (dir_ / "content.bin").string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return path;
  }
  std::string path_;
  std::filesystem::path dir_;
  static inline std::uint32_t seq_ = 0;
};

TEST_F(FileEndpointAdapterTest,
       AdmitOpenThenSubmitReadPublishesPayloadAndObservation) {
  const auto path = makeFile("hello world");
  FileEndpointFixture fixture;

  const auto opened =
      openAndAwait(fixture, path, fileRightRead, ioConsistencyLive);
  ASSERT_TRUE(opened.has_value());
  EXPECT_TRUE(fixture.adapter.openedCount() == 1);
  const auto caps = fixture.adapter.capabilities(openOf(fixture, opened));
  ASSERT_TRUE(caps.has_value());
  EXPECT_EQ(caps->locator, path);
  EXPECT_EQ(caps->consistency, SourceConsistency::Live);
  EXPECT_EQ(caps->identity, FileSourceIdentity::OpenedInode);

  // Stage a read under a live spec cell and submit it on the opened
  // capability: the full admitOpen -> submit -> worker -> publish path.
  const auto spec = stageSpec(
      fixture, fileReadSpecTag, openOf(fixture, opened),
      FileOpSpec{.kind = FileOpSpec::Kind::Read, .range = MediaRange{6, 5}});
  const auto outcome = submitAndAwait(fixture, opened->initialCapability, spec);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *outcome), IoOutcomeTag::Complete);

  const auto bytes = readOutcomePayload(fixture, *outcome);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "world");

  // The outcome links a real ReadObservation cell binding the same bytes.
  const auto observation =
      outcomeCellReadObservation(fixture.arena, fixture.schema, *outcome);
  ASSERT_TRUE(observation.has_value());
  EXPECT_EQ(observation->observationId, 1U);
  EXPECT_EQ(observation->locator, path);
  EXPECT_EQ(observation->consistency, SourceConsistency::Live);
  EXPECT_EQ(observation->offset, 6U);
  EXPECT_EQ(observation->length, 5U);
  ASSERT_TRUE(observation->payload.has_value());
  EXPECT_EQ(observation->payload->length, 5U);
}

TEST_F(FileEndpointAdapterTest, AdmitOpenThenSubmitWriteUpdatesFile) {
  const auto path = makeFile("0123456789");
  FileEndpointFixture fixture;

  const auto opened = openAndAwait(
      fixture, path, fileRightRead | fileRightWrite, ioConsistencyLive);
  ASSERT_TRUE(opened.has_value());

  const auto spec =
      stageSpec(fixture, fileWriteSpecTag, openOf(fixture, opened),
                FileOpSpec{.kind       = FileOpSpec::Kind::Write,
                           .range      = MediaRange{2, 0},
                           .writeBytes = bytesOf("XY")});
  const auto outcome = submitAndAwait(fixture, opened->initialCapability, spec);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *outcome), IoOutcomeTag::Complete);

  std::ifstream in(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "01XY456789");
}

TEST_F(FileEndpointAdapterTest, OpenOfMissingFileFailsWithTypedOutcome) {
  const auto path = (dir_ / "missing.bin").string();
  FileEndpointFixture fixture;

  OpenSpec spec{.adapterKey      = fileOpenAdapterKey,
                .locator         = bytesOf(path),
                .requestedRights = fileRightRead,
                .consistency     = ioConsistencyLive,
                .deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5)};
  const auto submitted = fixture.service->admitOpen(spec);
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto outcome = awaitOutcome(
      *fixture.service, std::get<Admitted>(submitted.value).request);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *outcome), IoOutcomeTag::Failed);
  EXPECT_EQ(fixture.adapter.openedCount(), 0U);
}

TEST_F(FileEndpointAdapterTest, OpenOfAFifoIsRefusedWithoutBlockingTheWorker) {
  const auto path = (dir_ / "pipe").string();
  ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
  FileEndpointFixture fixture;

  OpenSpec spec{.adapterKey      = fileOpenAdapterKey,
                .locator         = bytesOf(path),
                .requestedRights = fileRightRead,
                .consistency     = ioConsistencyLive,
                .deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5)};
  const auto submitted = fixture.service->admitOpen(spec);
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto outcome = awaitOutcome(
      *fixture.service, std::get<Admitted>(submitted.value).request);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *outcome), IoOutcomeTag::Failed);
  EXPECT_EQ(fixture.adapter.openedCount(), 0U);
}

TEST_F(FileEndpointAdapterTest,
       StableConsistencyRequestIsRefusedBeforeAdmission) {
  const auto path = makeFile("stable please");
  FileEndpointFixture fixture;

  // The grant allows only Live; a Stable-only request fails the grant's
  // consistency subset check at authorizeOpen, before any adapter work.
  OpenSpec spec{.adapterKey      = fileOpenAdapterKey,
                .locator         = bytesOf(path),
                .requestedRights = fileRightRead,
                .consistency     = ioConsistencyStable,
                .deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5)};
  const auto submitted = fixture.service->admitOpen(spec);
  ASSERT_TRUE(std::holds_alternative<Refused>(submitted.value));
  const auto outcome = std::get<Refused>(submitted.value).outcome;
  EXPECT_EQ(ioOutcomeTag(fixture.arena, outcome), IoOutcomeTag::Rejected);
  EXPECT_EQ(fixture.adapter.openedCount(), 0U);
}

TEST_F(FileEndpointAdapterTest, WriteThroughReadOnlyOpenIsRefusedUnsupported) {
  const auto path = makeFile("read only here");
  FileEndpointFixture fixture;

  // Open WITHOUT the write right: the descriptor is O_RDONLY.
  const auto opened =
      openAndAwait(fixture, path, fileRightRead, ioConsistencyLive);
  ASSERT_TRUE(opened.has_value());

  const auto spec =
      stageSpec(fixture, fileWriteSpecTag, openOf(fixture, opened),
                FileOpSpec{.kind       = FileOpSpec::Kind::Write,
                           .range      = MediaRange{0, 0},
                           .writeBytes = bytesOf("nope")});
  const auto outcome = submitAndAwait(fixture, opened->initialCapability, spec);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *outcome), IoOutcomeTag::Rejected);
}

TEST_F(FileEndpointAdapterTest, StageAtRefusesAnOpenThatDoesNotExist) {
  FileEndpointFixture fixture;
  const auto spec   = fixture.arena.makeCell(fileReadSpecTag);
  const auto staged = fixture.adapter.stageAt(
      spec, FileEndpointOp{.open = FileOpenId{999},
                           .op   = FileOpSpec{.kind  = FileOpSpec::Kind::Read,
                                              .range = MediaRange{0, 1}}});
  ASSERT_FALSE(staged);
}

TEST_F(FileEndpointAdapterTest, TwoFilesShareOneAdapterAndDispatchByOpen) {
  const auto pathA = makeFile("alpha");
  const auto pathB = (dir_ / "b.bin").string();
  {
    std::ofstream out(pathB, std::ios::binary | std::ios::trunc);
    out << "beta";
  }
  FileEndpointFixture fixture;

  const auto openedA =
      openAndAwait(fixture, pathA, fileRightRead, ioConsistencyLive);
  const auto openedB =
      openAndAwait(fixture, pathB, fileRightRead, ioConsistencyLive);
  ASSERT_TRUE(openedA.has_value());
  ASSERT_TRUE(openedB.has_value());
  EXPECT_EQ(fixture.adapter.openedCount(), 2U);

  const auto specA = stageSpec(
      fixture, fileReadSpecTag, openOf(fixture, openedA),
      FileOpSpec{.kind = FileOpSpec::Kind::Read, .range = MediaRange{0, 5}});
  const auto specB = stageSpec(
      fixture, fileReadSpecTag, openOf(fixture, openedB),
      FileOpSpec{.kind = FileOpSpec::Kind::Read, .range = MediaRange{0, 4}});
  const auto outcomeA =
      submitAndAwait(fixture, openedA->initialCapability, specA);
  const auto outcomeB =
      submitAndAwait(fixture, openedB->initialCapability, specB);
  ASSERT_TRUE(outcomeA.has_value());
  ASSERT_TRUE(outcomeB.has_value());
  const auto bytesA = readOutcomePayload(fixture, *outcomeA);
  const auto bytesB = readOutcomePayload(fixture, *outcomeB);
  ASSERT_TRUE(bytesA.has_value());
  ASSERT_TRUE(bytesB.has_value());
  EXPECT_EQ(textOf(*bytesA), "alpha");
  EXPECT_EQ(textOf(*bytesB), "beta");
}

TEST_F(FileEndpointAdapterTest, TwoOpensOfOnePathAcrossReplaceKeepTheirInodes) {
  const auto path = makeFile("inode-A");
  FileEndpointFixture fixture;

  const auto openedA =
      openAndAwait(fixture, path, fileRightRead, ioConsistencyLive);
  ASSERT_TRUE(openedA.has_value());
  const auto idA = openOf(fixture, openedA);

  // Replace the file under the same path, then open it again.
  const auto replacement = (dir_ / "replacement.bin").string();
  {
    std::ofstream out(replacement, std::ios::binary | std::ios::trunc);
    out << "inode-B";
  }
  ASSERT_EQ(::rename(replacement.c_str(), path.c_str()), 0);
  const auto openedB =
      openAndAwait(fixture, path, fileRightRead, ioConsistencyLive);
  ASSERT_TRUE(openedB.has_value());
  const auto idB = openOf(fixture, openedB);
  EXPECT_NE(idA, idB);
  EXPECT_EQ(fixture.adapter.openedCount(), 2U);

  // Stage on the FIRST open before and after the second open exists: both
  // still reach inode A.
  const auto specA = stageSpec(
      fixture, fileReadSpecTag, idA,
      FileOpSpec{.kind = FileOpSpec::Kind::Read, .range = MediaRange{0, 7}});
  const auto specB = stageSpec(
      fixture, fileReadSpecTag, idB,
      FileOpSpec{.kind = FileOpSpec::Kind::Read, .range = MediaRange{0, 7}});
  const auto outcomeA =
      submitAndAwait(fixture, openedA->initialCapability, specA);
  const auto outcomeB =
      submitAndAwait(fixture, openedB->initialCapability, specB);
  ASSERT_TRUE(outcomeA.has_value());
  ASSERT_TRUE(outcomeB.has_value());
  const auto bytesA = readOutcomePayload(fixture, *outcomeA);
  const auto bytesB = readOutcomePayload(fixture, *outcomeB);
  ASSERT_TRUE(bytesA.has_value());
  ASSERT_TRUE(bytesB.has_value());
  EXPECT_EQ(textOf(*bytesA), "inode-A");
  EXPECT_EQ(textOf(*bytesB), "inode-B");
}

TEST_F(FileEndpointAdapterTest, ReleaseClosesTheOpenAndRefusesFurtherUse) {
  const auto path = makeFile("release me");
  FileEndpointFixture fixture;
  const auto fdCount = [] {
    std::size_t n = 0;
    for ([[maybe_unused]] const auto &entry :
         std::filesystem::directory_iterator("/proc/self/fd"))
      ++n;
    return n;
  };
  const auto baseline = fdCount();

  const auto opened =
      openAndAwait(fixture, path, fileRightRead, ioConsistencyLive);
  ASSERT_TRUE(opened.has_value());
  const auto id = openOf(fixture, opened);
  EXPECT_EQ(fdCount(), baseline + 1);

  // A staged op dies with its open; the key is free again afterwards.
  const auto spec = fixture.arena.makeCell(fileReadSpecTag);
  ASSERT_TRUE(fixture.adapter.stageAt(
      spec, FileEndpointOp{.open = id,
                           .op   = FileOpSpec{.kind  = FileOpSpec::Kind::Read,
                                              .range = MediaRange{0, 1}}}));

  // An in-flight holder keeps the descriptor open past release.
  auto held = fixture.adapter.fileFor(id);
  ASSERT_NE(held, nullptr);
  const auto released = fixture.adapter.release(id);
  ASSERT_TRUE(released);
  EXPECT_EQ(*released, &fixture.adapter);
  EXPECT_EQ(fixture.adapter.openedCount(), 0U);
  EXPECT_FALSE(fixture.adapter.openIdOf(opened->endpoint).has_value());
  EXPECT_FALSE(fixture.adapter.capabilities(id).has_value());
  EXPECT_EQ(fdCount(), baseline + 1);
  EXPECT_TRUE(held->probeCapabilities().has_value());
  held.reset();
  EXPECT_EQ(fdCount(), baseline);

  EXPECT_FALSE(fixture.adapter.release(id));
  EXPECT_FALSE(fixture.adapter.stageAt(
      spec, FileEndpointOp{.open = id, .op = FileOpSpec{}}));
  // The submit of the dropped staged op is refused at prepare.
  const auto outcome = submitAndAwait(fixture, opened->initialCapability, spec);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *outcome), IoOutcomeTag::Rejected);
}

TEST_F(FileEndpointAdapterTest, DuplicateLiveStageKeyAcrossOpensIsRefused) {
  const auto pathA = makeFile("alpha");
  const auto pathB = (dir_ / "b.bin").string();
  {
    std::ofstream out(pathB, std::ios::binary | std::ios::trunc);
    out << "beta";
  }
  FileEndpointFixture fixture;
  const auto openedA =
      openAndAwait(fixture, pathA, fileRightRead, ioConsistencyLive);
  const auto openedB =
      openAndAwait(fixture, pathB, fileRightRead, ioConsistencyLive);
  const auto idA = openOf(fixture, openedA);
  const auto idB = openOf(fixture, openedB);

  const auto spec = fixture.arena.makeCell(fileReadSpecTag);
  const FileOpSpec read{.kind = FileOpSpec::Kind::Read, .range = {0, 1}};
  ASSERT_TRUE(fixture.adapter.stageAt(spec, {.open = idA, .op = read}));
  // The same key against the OTHER file must not overwrite the route.
  EXPECT_FALSE(fixture.adapter.stageAt(spec, {.open = idB, .op = read}));
  EXPECT_FALSE(fixture.adapter.stageAt(spec, {.open = idA, .op = read}));

  // Abandoning frees the key for reuse.
  fixture.adapter.abandon(spec);
  EXPECT_TRUE(fixture.adapter.stageAt(spec, {.open = idB, .op = read}));
}

/// Open @p path without awaiting: the request cell, for `awaitOutcome` later.
std::optional<RequestCell> startOpen(FileEndpointFixture &fixture,
                                     const std::string &path) {
  const auto submitted = fixture.service->admitOpen(OpenSpec{
      .adapterKey      = fileOpenAdapterKey,
      .locator         = bytesOf(path),
      .requestedRights = fileRightRead,
      .consistency     = ioConsistencyLive,
      .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5)});
  if (!std::holds_alternative<Admitted>(submitted.value)) return std::nullopt;
  return std::get<Admitted>(submitted.value).request;
}

std::size_t openFdCount() {
  std::size_t n = 0;
  for ([[maybe_unused]] const auto &entry :
       std::filesystem::directory_iterator("/proc/self/fd"))
    ++n;
  return n;
}

TEST_F(FileEndpointAdapterTest, ConcurrentOpensOfOnePathBindTheirOwnInodes) {
  const auto path = makeFile("inode-A");
  FileEndpointFixture fixture;

  // Both opens are in flight before either is awaited; the replace lands
  // between them, so completion order is the only thing that could mix them.
  const auto first = startOpen(fixture, path);
  ASSERT_TRUE(first.has_value());
  const auto firstOutcome = awaitOutcome(*fixture.service, *first);
  ASSERT_TRUE(firstOutcome.has_value());
  const auto replacement = (dir_ / "replacement.bin").string();
  {
    std::ofstream out(replacement, std::ios::binary | std::ios::trunc);
    out << "inode-B";
  }
  ASSERT_EQ(::rename(replacement.c_str(), path.c_str()), 0);
  const auto second = startOpen(fixture, path);
  const auto third  = startOpen(fixture, path);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());
  const auto thirdOutcome  = awaitOutcome(*fixture.service, *third);
  const auto secondOutcome = awaitOutcome(*fixture.service, *second);
  ASSERT_TRUE(secondOutcome.has_value());
  ASSERT_TRUE(thirdOutcome.has_value());

  const auto openedA =
      openedEndpointOf(fixture.schema, fixture.arena, *firstOutcome);
  const auto openedB =
      openedEndpointOf(fixture.schema, fixture.arena, *secondOutcome);
  const auto openedC =
      openedEndpointOf(fixture.schema, fixture.arena, *thirdOutcome);
  ASSERT_TRUE(openedA && openedB && openedC);
  const auto idA = openOf(fixture, openedA);
  const auto idB = openOf(fixture, openedB);
  const auto idC = openOf(fixture, openedC);
  EXPECT_NE(idA, idB);
  EXPECT_NE(idB, idC);
  EXPECT_NE(idA, idC);
  EXPECT_EQ(fixture.adapter.openedCount(), 3U);

  const auto readOf = [&](const std::optional<OpenedEndpoint> &opened,
                          const FileOpenId id) {
    const auto spec = stageSpec(
        fixture, fileReadSpecTag, id,
        FileOpSpec{.kind = FileOpSpec::Kind::Read, .range = MediaRange{0, 7}});
    const auto outcome =
        submitAndAwait(fixture, opened->initialCapability, spec);
    const auto bytes = outcome ? readOutcomePayload(fixture, *outcome)
                               : std::unexpected(IoError{});
    return bytes ? textOf(*bytes) : std::string{};
  };
  EXPECT_EQ(readOf(openedA, idA), "inode-A");
  EXPECT_EQ(readOf(openedB, idB), "inode-B");
  EXPECT_EQ(readOf(openedC, idC), "inode-B");
}

TEST_F(FileEndpointAdapterTest, CloseEndpointReleasesTheDescriptor) {
  const auto path     = makeFile("close me");
  const auto baseline = openFdCount();
  FileEndpointFixture fixture;
  const auto opened =
      openAndAwait(fixture, path, fileRightRead, ioConsistencyLive);
  ASSERT_TRUE(opened.has_value());
  const auto id = openOf(fixture, opened);
  EXPECT_EQ(openFdCount(), baseline + 1);

  const auto close =
      fixture.service->closeEndpoint(opened->endpoint, CloseMode::Graceful);
  ASSERT_TRUE(std::holds_alternative<Admitted>(close.value));
  const auto outcome =
      awaitOutcome(*fixture.service, std::get<Admitted>(close.value).request);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *outcome), IoOutcomeTag::Complete);
  EXPECT_EQ(openFdCount(), baseline);
  EXPECT_EQ(fixture.adapter.openedCount(), 0U);
  EXPECT_FALSE(fixture.adapter.openIdOf(opened->endpoint).has_value());
  EXPECT_FALSE(fixture.adapter.capabilities(id).has_value());

  // Later work on the closed endpoint refuses.
  EXPECT_FALSE(
      fixture.adapter.stageAt(fixture.arena.makeCell(fileReadSpecTag),
                              FileEndpointOp{.open = id, .op = FileOpSpec{}}));
  const auto spec = fixture.arena.makeCell(fileReadSpecTag);
  const auto late = fixture.service->submit(opened->initialCapability, spec);
  EXPECT_TRUE(std::holds_alternative<Refused>(late.value));
}

TEST_F(FileEndpointAdapterTest, SecondCloseIsRefusedTyped) {
  const auto path = makeFile("close twice");
  FileEndpointFixture fixture;
  const auto opened =
      openAndAwait(fixture, path, fileRightRead, ioConsistencyLive);
  ASSERT_TRUE(opened.has_value());
  const auto first =
      fixture.service->closeEndpoint(opened->endpoint, CloseMode::Graceful);
  ASSERT_TRUE(std::holds_alternative<Admitted>(first.value));
  ASSERT_TRUE(
      awaitOutcome(*fixture.service, std::get<Admitted>(first.value).request));

  const auto second =
      fixture.service->closeEndpoint(opened->endpoint, CloseMode::Graceful);
  ASSERT_TRUE(std::holds_alternative<Refused>(second.value));
  EXPECT_EQ(
      ioOutcomeTag(fixture.arena, std::get<Refused>(second.value).outcome),
      IoOutcomeTag::Rejected);

  // Below the service's tombstone the adapter refuses too: no binding is
  // left, so the decoder cannot even freeze a second close.
  EXPECT_FALSE(makeFileCloseDecoder(fixture.adapter)(opened->endpoint,
                                                     CloseMode::Graceful));
  EXPECT_FALSE(fixture.adapter.release(FileOpenId{1}));
}

TEST_F(FileEndpointAdapterTest, CloseWithAnOpInFlightKeepsTheFileUntilItEnds) {
  const auto path     = makeFile("in flight");
  const auto baseline = openFdCount();
  FileEndpointFixture fixture;
  const auto opened =
      openAndAwait(fixture, path, fileRightRead, ioConsistencyLive);
  ASSERT_TRUE(opened.has_value());
  const auto id = openOf(fixture, opened);

  // An executing op holds the file exactly as this holder does.
  auto inFlight = fixture.adapter.fileFor(id);
  ASSERT_NE(inFlight, nullptr);
  const auto close =
      fixture.service->closeEndpoint(opened->endpoint, CloseMode::Graceful);
  ASSERT_TRUE(std::holds_alternative<Admitted>(close.value));
  ASSERT_TRUE(
      awaitOutcome(*fixture.service, std::get<Admitted>(close.value).request));
  EXPECT_EQ(fixture.adapter.openedCount(), 0U);
  EXPECT_EQ(openFdCount(), baseline + 1);
  EXPECT_TRUE(inFlight->probeCapabilities().has_value());
  inFlight.reset();
  EXPECT_EQ(openFdCount(), baseline);
}

// ---- M7-VQL-c C1: open modes ---------------------------------------------

/// Admit an open and return its terminal outcome even when it ended without
/// minting an endpoint (openAndAwait collapses that to nullopt).
std::optional<OutcomeCell> openOutcome(FileEndpointFixture &fixture,
                                       const std::string &path,
                                       std::uint64_t rights) {
  OpenSpec spec{.adapterKey      = fileOpenAdapterKey,
                .locator         = bytesOf(path),
                .requestedRights = rights,
                .consistency     = ioConsistencyLive,
                .deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5)};
  const auto submitted = fixture.service->admitOpen(spec);
  if (const auto *refused = std::get_if<Refused>(&submitted.value))
    return refused->outcome;
  return awaitOutcome(*fixture.service,
                      std::get<Admitted>(submitted.value).request);
}

TEST_F(FileEndpointAdapterTest, WriteOpenRefusesASymlinkAtTheFinalComponent) {
  const auto target = makeFile("target");
  const auto link   = (dir_ / "link.bin").string();
  std::filesystem::create_symlink(target, link);
  FileEndpointFixture fixture;

  // The host checks a path before the worker opens it; O_NOFOLLOW is what
  // stops a link swapped in between from redirecting the write.
  for (const auto rights :
       {fileRightWrite, fileRightAppend, fileRightWrite | fileRightCreate}) {
    const auto outcome = openOutcome(fixture, link, rights);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(ioOutcomeTag(fixture.arena, *outcome), IoOutcomeTag::Failed);
  }
  EXPECT_EQ(fixture.adapter.openedCount(), 0U);
  std::ifstream in(target, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "target");

  // A read open follows it, as a read always has.
  const auto read =
      openAndAwait(fixture, link, fileRightRead, ioConsistencyLive);
  EXPECT_TRUE(read.has_value());
}

TEST_F(FileEndpointAdapterTest, CreateOpenMakesAPrivateFileAndNeverClobbers) {
  const auto path = (dir_ / "created.bin").string();
  FileEndpointFixture fixture;

  const auto first = openAndAwait(
      fixture, path, fileRightWrite | fileRightCreate, ioConsistencyLive);
  ASSERT_TRUE(first.has_value());
  struct stat st{};
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777U, 0600U);
  const auto wrote = submitAndAwait(
      fixture, first->initialCapability,
      stageSpec(fixture, fileWriteSpecTag, openOf(fixture, first),
                FileOpSpec{.kind       = FileOpSpec::Kind::Write,
                           .writeBytes = bytesOf("kept")}));
  ASSERT_TRUE(wrote.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *wrote), IoOutcomeTag::Complete);

  const auto second =
      openOutcome(fixture, path, fileRightWrite | fileRightCreate);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *second), IoOutcomeTag::Failed);
  std::ifstream in(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "kept");
}

TEST_F(FileEndpointAdapterTest, AppendOpenWritesAtTheEndAndRefusesPositional) {
  const auto path = makeFile("one,");
  FileEndpointFixture fixture;
  const auto opened =
      openAndAwait(fixture, path, fileRightAppend, ioConsistencyLive);
  ASSERT_TRUE(opened.has_value());

  const auto added = submitAndAwait(
      fixture, opened->initialCapability,
      stageSpec(fixture, fileAppendSpecTag, openOf(fixture, opened),
                FileOpSpec{.kind       = FileOpSpec::Kind::Append,
                           .writeBytes = bytesOf("two,")}));
  ASSERT_TRUE(added.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *added), IoOutcomeTag::Complete);
  const auto positional = submitAndAwait(
      fixture, opened->initialCapability,
      stageSpec(fixture, fileWriteSpecTag, openOf(fixture, opened),
                FileOpSpec{.kind       = FileOpSpec::Kind::Write,
                           .writeBytes = bytesOf("X")}));
  ASSERT_TRUE(positional.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *positional), IoOutcomeTag::Rejected);

  // An append capability can still be read back through the same endpoint.
  const auto read = submitAndAwait(
      fixture, opened->initialCapability,
      stageSpec(fixture, fileReadSpecTag, openOf(fixture, opened),
                FileOpSpec{.kind  = FileOpSpec::Kind::Read,
                           .range = MediaRange{0, 16}}));
  ASSERT_TRUE(read.has_value());
  const auto bytes = readOutcomePayload(fixture, *read);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "one,two,");
}

TEST_F(FileEndpointAdapterTest, WriteCommandBytesAreChargedToTheCommandBudget) {
  const auto path = makeFile("");
  FileEndpointFixture fixture;
  const auto opened =
      openAndAwait(fixture, path, fileRightWrite, ioConsistencyLive);
  ASSERT_TRUE(opened.has_value());

  // The fixture's service allows 4 MiB of command bytes; a 5 MiB write must be
  // refused at admission and leave the file untouched.
  const auto spec =
      stageSpec(fixture, fileWriteSpecTag, openOf(fixture, opened),
                FileOpSpec{.kind       = FileOpSpec::Kind::Write,
                           .writeBytes = std::vector<std::byte>(5U << 20)});
  const auto outcome = submitAndAwait(fixture, opened->initialCapability, spec);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(fixture.arena, *outcome), IoOutcomeTag::Rejected);
  EXPECT_EQ(std::filesystem::file_size(path), 0U);
}

} // namespace

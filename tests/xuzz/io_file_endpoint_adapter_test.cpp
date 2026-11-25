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
              {},
              [](const OpenSpec &)
                  -> std::expected<ArenaIoCellStore::OpenGrant, IoErrorInfo> {
                return ArenaIoCellStore::OpenGrant{
                    .rights      = fileRightRead | fileRightWrite,
                    .consistency = ioConsistencyLive};
              }) {
    scheduler = std::make_unique<BoundedIoScheduler>(
        BoundedIoSchedulerOptions{2, 8, 1 << 22, 1 << 22}, payloads);
    service = std::make_unique<IoService>(
        IoBudgetSnapshot{16, 1 << 22, 1 << 22, 1 << 22, 8},
        std::move(scheduler), payloads, cells);
    service->registerAdapter(fileOpenAdapterKey, adapter);
    service->registerAdapter(fileDataAdapterKey, adapter);
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

/// The newest live open of @p path -- the one the caller just awaited.
FileOpenId lastOpen(const FileEndpointFixture &fixture,
                    const std::string &path) {
  const auto ids = fixture.adapter.openIdsFor(path);
  EXPECT_FALSE(ids.empty());
  return ids.empty() ? FileOpenId{} : ids.back();
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
  const auto caps = fixture.adapter.capabilities(lastOpen(fixture, path));
  ASSERT_TRUE(caps.has_value());
  EXPECT_EQ(caps->locator, path);
  EXPECT_EQ(caps->consistency, SourceConsistency::Live);
  EXPECT_EQ(caps->identity, FileSourceIdentity::OpenedInode);

  // Stage a read under a live spec cell and submit it on the opened
  // capability: the full admitOpen -> submit -> worker -> publish path.
  const auto spec = stageSpec(
      fixture, fileReadSpecTag, lastOpen(fixture, path),
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
      stageSpec(fixture, fileWriteSpecTag, lastOpen(fixture, path),
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
      stageSpec(fixture, fileWriteSpecTag, lastOpen(fixture, path),
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
      fixture, fileReadSpecTag, lastOpen(fixture, pathA),
      FileOpSpec{.kind = FileOpSpec::Kind::Read, .range = MediaRange{0, 5}});
  const auto specB = stageSpec(
      fixture, fileReadSpecTag, lastOpen(fixture, pathB),
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
  const auto idA = lastOpen(fixture, path);

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
  const auto ids = fixture.adapter.openIdsFor(path);
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids.front(), idA);
  const auto idB = ids.back();
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
  const auto id = lastOpen(fixture, path);
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
  EXPECT_TRUE(fixture.adapter.openIdsFor(path).empty());
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
  ASSERT_TRUE(openAndAwait(fixture, pathA, fileRightRead, ioConsistencyLive));
  ASSERT_TRUE(openAndAwait(fixture, pathB, fileRightRead, ioConsistencyLive));
  const auto idA = lastOpen(fixture, pathA);
  const auto idB = lastOpen(fixture, pathB);

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

} // namespace

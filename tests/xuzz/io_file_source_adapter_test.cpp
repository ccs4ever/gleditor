#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/xanadu/vortex/file_source_adapter.hpp"

namespace {
using namespace xanadu::vortex;

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

struct CapturingSink final : CompletionSink {
  /// @p resolver mirrors the service sink: the terminal event retains the
  /// payload so the bytes outlive the worker lease `execute`'s guard
  /// releases on return. Pass the same PayloadResolver the BackendContext
  /// carries.
  explicit CapturingSink(PayloadResolver *resolver = nullptr)
      : payloads(resolver) {}
  void progress(PrivateRequestToken, IoProgress) noexcept override {}
  void
  terminalCandidate(PrivateRequestToken, BackendResult result,
                    std::optional<PayloadLease> payload) noexcept override {
    candidate = std::move(result);
    if (payload && payloads) {
      if (auto retained = payloads->retain(payload->range)) {
        payloadLease = *retained;
      }
    }
  }
  void releaseAcknowledged(BackendReleaseAck ack) noexcept override {
    released = ack;
  }
  PayloadResolver *payloads;
  std::optional<BackendResult> candidate;
  std::optional<PayloadLease> payloadLease;
  std::optional<BackendReleaseAck> released;
};

BackendContext makeContext(PayloadResolver &payloads,
                           std::stop_token stop = {}) {
  return BackendContext{
      .stop = stop, .payloads = payloads, .budget = {1 << 20}};
}

/// Read every byte a terminal outcome's payload lease retains. Byte output
/// travels the payload channel (design §3.2), never IoResultValue.
std::expected<std::vector<std::byte>, IoError>
readPayload(PayloadResolver &payloads, const PayloadLease &lease) {
  return payloads.read(lease, 0, static_cast<std::size_t>(lease.range.length));
}

CellRequest requestFor(std::uint32_t specification) {
  return CellRequest{.key           = AdapterKey{10, 1},
                     .capability    = CapabilityCell{},
                     .specification = specification};
}

/// A fixture directory under the test's own XDG cache, never the author's
/// real data (repository policy: tests stay headless and off the real
/// permascroll/config/cache).
class FileSourceAdapterTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto base =
        std::filesystem::temp_directory_path() /
        ("io_file_source_adapter_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(base);
    dir_  = base;
    path_ = (base / "content.bin").string();
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  void writeFile(std::string_view content) const {
    FILE *f = std::fopen(path_.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
  }

  std::filesystem::path dir_;
  std::string path_;
};

TEST_F(FileSourceAdapterTest, ProbeCapabilitiesDeclaresLiveNeverStable) {
  writeFile("hello world");
  FileSourceAdapter adapter(path_);
  const auto caps = adapter.probeCapabilities();
  ASSERT_TRUE(caps.has_value());
  EXPECT_EQ(caps->consistency, SourceConsistency::Live);
  EXPECT_EQ(caps->sizeAtOpen, 11U);
}

TEST_F(FileSourceAdapterTest, ReadSucceedsAndRecordsDistinctObservation) {
  writeFile("hello world");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto key =
      adapter.stageRead(MediaRange{0, 5}, adapter.currentGeneration());
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *complete = std::get_if<Complete>(&sink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  ASSERT_TRUE(sink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *sink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "hello");
  ASSERT_TRUE(sink.released);

  // Two reads of the identical range still get distinct observation ids: a
  // Live source never claims the backend-proven identity that would license
  // collapsing them (plan §7.2).
  const auto key2 =
      adapter.stageRead(MediaRange{0, 5}, adapter.currentGeneration());
  const auto prepared2 = adapter.prepare(requestFor(key2), owner);
  ASSERT_TRUE(prepared2);
  CapturingSink sink2{&payloads};
  adapter.execute(BackendCommand{{2}, *prepared2}, context, sink2);
  ASSERT_TRUE(sink2.candidate);
  ASSERT_TRUE(std::holds_alternative<Complete>(sink2.candidate->outcome));

  EXPECT_TRUE(adapter.observation(1).has_value());
  EXPECT_TRUE(adapter.observation(2).has_value());
  EXPECT_NE(adapter.observation(1)->observationId,
            adapter.observation(2)->observationId);
}

TEST_F(FileSourceAdapterTest, ReadAtOrPastEndOfFileIsEndOfStream) {
  writeFile("ab");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto key      = adapter.stageRead(MediaRange{2, 10}, 0);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<EndOfStream>(sink.candidate->outcome));
}

TEST_F(FileSourceAdapterTest, ReadBeyondBudgetIsBounded) {
  writeFile(std::string(4096, 'x'));
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  BackendContext context{
      .stop = {}, .payloads = payloads, .budget = BackendBudget{16}};

  const auto key      = adapter.stageRead(MediaRange{0, 4096}, 0);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *complete = std::get_if<Complete>(&sink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  ASSERT_TRUE(sink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *sink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  // The adapter must not allocate an unbounded staging buffer ahead of the
  // declared budget (io_protocol.hpp's IoAdapter contract).
  EXPECT_LE(bytes->size(), 16U);
}

TEST_F(FileSourceAdapterTest,
       OversizedRangeClampsToFileSizeInsteadOfAllocatingIt) {
  writeFile("hello");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  // maxBytes == 0 reads as unbounded: without the file-size clamp the
  // staging vector would try to allocate the full 1 TiB range before the
  // first pread ever saw the EOF.
  BackendContext context{
      .stop = {}, .payloads = payloads, .budget = BackendBudget{0}};

  const auto key = adapter.stageRead(MediaRange{0, std::uint64_t{1} << 40}, 0);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *complete = std::get_if<Complete>(&sink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  ASSERT_TRUE(sink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *sink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "hello");
}

// -- in-place mutation / external writer -------------------------------------

TEST_F(FileSourceAdapterTest,
       PrepareRefusesStaleGenerationAfterExternalMutation) {
  writeFile("v1");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  const auto staleGeneration = adapter.currentGeneration();

  // An external writer (another fd, same path) replaces the content in
  // place; `stat`'s size/mtime move even though the inode does not.
  {
    const int fd = ::open(path_.c_str(), O_WRONLY | O_TRUNC);
    ASSERT_GE(fd, 0);
    const char data[] = "v2x";
    ASSERT_EQ(::write(fd, data, sizeof(data) - 1),
              static_cast<ssize_t>(sizeof(data) - 1));
    ::close(fd);
  }

  const auto key      = adapter.stageRead(MediaRange{0, 2}, staleGeneration);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_FALSE(prepared);
  EXPECT_EQ(prepared.error().code, IoErrorCode::Stale);
}

TEST_F(FileSourceAdapterTest,
       ExecuteReportsConflictWhenExternalWriterRacesAdmission) {
  writeFile("before-content");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto key =
      adapter.stageRead(MediaRange{0, 6}, adapter.currentGeneration());
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);

  // Between admission and the worker turn, an external writer (not this
  // adapter) mutates the file.
  {
    const int fd = ::open(path_.c_str(), O_WRONLY | O_TRUNC);
    ASSERT_GE(fd, 0);
    const char data[] = "raced!!";
    ASSERT_EQ(::write(fd, data, sizeof(data) - 1),
              static_cast<ssize_t>(sizeof(data) - 1));
    ::close(fd);
  }

  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<Conflict>(sink.candidate->outcome));
}

// -- truncation ---------------------------------------------------------------

TEST_F(FileSourceAdapterTest, TruncationBumpsGenerationAndShortensReads) {
  writeFile("0123456789");
  FileSourceAdapter adapter(path_);
  const auto before = adapter.currentGeneration();

  ASSERT_EQ(::truncate(path_.c_str(), 3), 0);
  const auto after = adapter.currentGeneration();
  EXPECT_NE(before, after);

  OwnerContext owner;
  PayloadResolver payloads;
  auto context        = makeContext(payloads);
  const auto key      = adapter.stageRead(MediaRange{0, 10}, 0);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *complete = std::get_if<Complete>(&sink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  ASSERT_TRUE(sink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *sink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "012");
}

// -- replacement (rename swaps the path to a new inode under an open fd) ----

TEST_F(FileSourceAdapterTest, HeldFdKeepsReadingUnlinkedOriginalAfterReplace) {
  writeFile("original");
  FileSourceAdapter adapter(path_);

  // Replace the path with a brand-new file; POSIX rename leaves any already
  // open fd (ours) pointing at the old (now unlinked) inode's content.
  const auto replacement = (dir_ / "replacement.bin").string();
  {
    FILE *f = std::fopen(replacement.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fwrite("replaced", 1, 8, f);
    std::fclose(f);
  }
  ASSERT_EQ(::rename(replacement.c_str(), path_.c_str()), 0);

  OwnerContext owner;
  PayloadResolver payloads;
  auto context        = makeContext(payloads);
  const auto key      = adapter.stageRead(MediaRange{0, 8}, 0);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *complete = std::get_if<Complete>(&sink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  ASSERT_TRUE(sink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *sink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "original");
}

// -- offsets above 4 GiB (sparse file; skip gracefully if unsupported) ------

TEST_F(FileSourceAdapterTest, ReadsPast4GiBOffsetOnASparseFile) {
  constexpr std::uint64_t fiveGiB = 5ULL * 1024 * 1024 * 1024;
  {
    FILE *f = std::fopen(path_.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fclose(f);
  }
  const int fd = ::open(path_.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  if (::ftruncate(fd, static_cast<off_t>(fiveGiB + 16)) != 0) {
    ::close(fd);
    GTEST_SKIP() << "filesystem does not support a sparse file this large";
  }
  const char marker[] = "past-4gib-marker";
  const auto written =
      ::pwrite(fd, marker, sizeof(marker) - 1, static_cast<off_t>(fiveGiB));
  ::close(fd);
  if (written != static_cast<ssize_t>(sizeof(marker) - 1))
    GTEST_SKIP() << "pwrite at a >4GiB offset is not supported here";

  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);
  const auto key =
      adapter.stageRead(MediaRange{fiveGiB, sizeof(marker) - 1}, 0);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *complete = std::get_if<Complete>(&sink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  ASSERT_TRUE(sink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *sink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "past-4gib-marker");
}

// -- no path-only cache reuse -------------------------------------------------

TEST_F(FileSourceAdapterTest, TwoAdaptersOverSamePathShareNoObservations) {
  writeFile("shared path, distinct adapters");
  FileSourceAdapter first(path_);
  FileSourceAdapter second(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto key = first.stageRead(MediaRange{0, 6}, first.currentGeneration());
  const auto prepared = first.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  first.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  ASSERT_TRUE(std::holds_alternative<Complete>(sink.candidate->outcome));

  // The path string is identical, but nothing is keyed on it alone: a
  // second adapter instance has never recorded that observation.
  EXPECT_TRUE(first.observation(1).has_value());
  EXPECT_FALSE(second.observation(1).has_value());
}

// -- bounds and cancellation cleanup ------------------------------------------

TEST_F(FileSourceAdapterTest,
       CancelledBeforeTransferStillReleasesAndRecordsNoObservation) {
  writeFile("cancel me");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;

  std::stop_source stopSource;
  stopSource.request_stop();
  auto context = makeContext(payloads, stopSource.get_token());

  const auto key      = adapter.stageRead(MediaRange{0, 5}, 0);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<Cancelled>(sink.candidate->outcome));
  ASSERT_TRUE(sink.released);
  EXPECT_FALSE(adapter.observation(1).has_value());
}

TEST_F(FileSourceAdapterTest, InvalidStagedKeyRefusesAndReleases) {
  writeFile("x");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  // No stageRead was ever called for this key: prepare must refuse, not
  // read out-of-bounds staged state.
  const auto prepared = adapter.prepare(requestFor(999), owner);
  ASSERT_FALSE(prepared);
  EXPECT_EQ(prepared.error().code, IoErrorCode::InvalidRequest);
}

// -- unsupported atomicity refuses --------------------------------------------

TEST_F(FileSourceAdapterTest, StableReadRequestIsRefusedBeforeAdmission) {
  writeFile("needs a proof this backend cannot give");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;

  const auto key      = adapter.stageStableRead(MediaRange{0, 4});
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_FALSE(prepared);
  EXPECT_EQ(prepared.error().code, IoErrorCode::Unsupported);
}

TEST_F(FileSourceAdapterTest, WriteToReadOnlyAdapterIsRefused) {
  writeFile("read only");
  FileSourceAdapter adapter(path_, /*writable=*/false);
  OwnerContext owner;

  const auto key      = adapter.stageWrite(0, bytesOf("nope"), 0);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_FALSE(prepared);
  EXPECT_EQ(prepared.error().code, IoErrorCode::Unsupported);
}

TEST_F(FileSourceAdapterTest, StageWriteByteBoundRefusesBeforeEntryCap) {
  writeFile("x");
  FileSourceAdapter adapter(path_, /*writable=*/true);

  // Each staged write carries its whole byte vector: the 4096-entry cap
  // alone would let the table pin unbounded memory, so stageWrite() also
  // bounds the total staged bytes and fails as key 0 past it.
  std::vector<std::uint32_t> keys;
  const auto chunk = std::vector<std::byte>(8U * 1024U * 1024U);
  for (int i = 0; i < 4096; ++i) {
    const auto key = adapter.stageWrite(0, chunk, 0);
    if (key == 0) break;
    keys.push_back(key);
  }
  EXPECT_FALSE(keys.empty());
  EXPECT_LT(keys.size(), 4096U); // refused on bytes, not on entries

  // Byte-free ops are unaffected by the write-byte bound.
  const auto readKey = adapter.stageRead(MediaRange{0, 1}, 0);
  EXPECT_NE(readKey, 0U);

  // Abandoning a staged write returns its bytes to the budget.
  adapter.abandon(keys.back());
  keys.pop_back();
  const auto again = adapter.stageWrite(0, chunk, 0);
  EXPECT_NE(again, 0U);
}

TEST_F(FileSourceAdapterTest, WritableAdapterWritesAndBumpsGeneration) {
  writeFile("0123456789");
  FileSourceAdapter adapter(path_, /*writable=*/true);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context      = makeContext(payloads);
  const auto before = adapter.currentGeneration();

  const auto key      = adapter.stageWrite(2, bytesOf("XY"), before);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  ASSERT_TRUE(std::holds_alternative<Complete>(sink.candidate->outcome));
  EXPECT_NE(adapter.currentGeneration(), before);

  const auto readKey      = adapter.stageRead(MediaRange{0, 10}, 0);
  const auto readPrepared = adapter.prepare(requestFor(readKey), owner);
  ASSERT_TRUE(readPrepared);
  CapturingSink readSink{&payloads};
  adapter.execute(BackendCommand{{2}, *readPrepared}, context, readSink);
  const auto *complete = std::get_if<Complete>(&readSink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  ASSERT_TRUE(readSink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *readSink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "01XY456789");
}

// -- bounded decoder/cache leases ---------------------------------------------

TEST_F(FileSourceAdapterTest, DecodeLeasesAreBoundedAndRefuseOverBudget) {
  writeFile("media bytes");
  FileSourceAdapter adapter(path_);
  adapter.setMaxDecodeLeases(2);

  const auto first = adapter.acquireDecodeLease(MediaRange{0, 4});
  ASSERT_TRUE(first);
  const auto second = adapter.acquireDecodeLease(MediaRange{4, 4});
  ASSERT_TRUE(second);
  EXPECT_EQ(adapter.liveDecodeLeases(), 2U);

  const auto third = adapter.acquireDecodeLease(MediaRange{8, 2});
  ASSERT_FALSE(third);
  EXPECT_EQ(third.error().code, IoErrorCode::BudgetExceeded);

  adapter.releaseDecodeLease(first->lease);
  EXPECT_EQ(adapter.liveDecodeLeases(), 1U);
  const auto fourth = adapter.acquireDecodeLease(MediaRange{8, 2});
  ASSERT_TRUE(fourth);
  EXPECT_EQ(adapter.liveDecodeLeases(), 2U);
}

TEST_F(FileSourceAdapterTest, DecodeLeaseRecordsGenerationAtAcquisitionTime) {
  writeFile("v1-content");
  FileSourceAdapter adapter(path_);
  const auto lease = adapter.acquireDecodeLease(MediaRange{0, 2});
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease->sourceGeneration, adapter.currentGeneration());

  ASSERT_EQ(::truncate(path_.c_str(), 2), 0);
  // The lease itself stays valid metadata; it is the caller's job to notice
  // the generation moved before trusting any cached decode output (plan
  // §7.2's "prior observations remain readable" rule, applied to a decode
  // product rather than the raw bytes).
  EXPECT_NE(adapter.currentGeneration(), lease->sourceGeneration);
}

TEST_F(FileSourceAdapterTest, ObservationCapEvictsOldestFirst) {
  writeFile("x");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  // maxRetainedObservations_ is 4096: one read past the cap must evict
  // the OLDEST observation (id 1), not an arbitrary map entry -- the
  // retention promise is "recent reads stay readable," so eviction order
  // is part of the contract.
  for (std::uint64_t i = 0; i < 4097; ++i) {
    const auto key      = adapter.stageRead(MediaRange{0, 0}, 0);
    const auto prepared = adapter.prepare(requestFor(key), owner);
    ASSERT_TRUE(prepared);
    CapturingSink sink{&payloads};
    adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
    ASSERT_TRUE(sink.candidate);
    ASSERT_TRUE(std::holds_alternative<Complete>(sink.candidate->outcome));
  }

  EXPECT_FALSE(adapter.observation(1).has_value());
  EXPECT_TRUE(adapter.observation(2).has_value());
  EXPECT_TRUE(adapter.observation(4096).has_value());
  EXPECT_TRUE(adapter.observation(4097).has_value());
}

// -- declared source identity (plan §7.2: locator + capabilities) -----------

TEST_F(FileSourceAdapterTest, OpenedInodeIdentitySurvivesRename) {
  writeFile("inode pinned");
  FileSourceAdapter adapter(path_);
  const auto caps = adapter.probeCapabilities();
  ASSERT_TRUE(caps.has_value());
  // The capability must SAY which identity semantics the source has: this
  // adapter holds one fd, so operations address the opened inode.
  EXPECT_EQ(caps->identity, FileSourceIdentity::OpenedInode);
  EXPECT_EQ(caps->consistency, SourceConsistency::Live);

  const auto generationBefore = adapter.currentGeneration();

  // Replace the path with a new inode; the held fd -- and therefore every
  // staged op -- stays on the original.
  const auto replacement = (dir_ / "renamed-over.bin").string();
  {
    FILE *f = std::fopen(replacement.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fwrite("other inode", 1, 11, f);
    std::fclose(f);
  }
  ASSERT_EQ(::rename(replacement.c_str(), path_.c_str()), 0);

  // fstat on the held fd sees the ORIGINAL inode: size, mtime and st_ino
  // are all unchanged by the rename, so the declared generation is too --
  // a CAS staged against it is still honoured.
  EXPECT_EQ(adapter.currentGeneration(), generationBefore);

  OwnerContext owner;
  PayloadResolver payloads;
  auto context        = makeContext(payloads);
  const auto key      = adapter.stageRead(MediaRange{0, 12}, generationBefore);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  ASSERT_TRUE(sink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *sink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "inode pinned");
}

// -- same-size in-place rewrite: the stat-generation blind spot -------------

TEST_F(FileSourceAdapterTest,
       SameSizeRewriteWithRestoredMtimeSharesTheGeneration) {
  writeFile("aaaa");
  FileSourceAdapter adapter(path_);
  const auto generationBefore = adapter.currentGeneration();

  // Rewrite the same 4 bytes in place, then restore the exact timestamps:
  // size, mtime and inode all match the pre-rewrite stat, so
  // generationFromStat cannot tell the content moved. This is the documented
  // blind spot -- it is WHY Stable/CAS is refused rather than synthesized
  // from stat (file_source_adapter.cpp's generationFromStat comment).
  int fd = ::open(path_.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  struct stat before{};
  ASSERT_EQ(::fstat(fd, &before), 0);
  ASSERT_EQ(::pwrite(fd, "bbbb", 4, 0), 4);
  const std::array times{before.st_atim, before.st_mtim};
  ASSERT_EQ(::futimens(fd, times.data()), 0);
  ::close(fd);

  EXPECT_EQ(adapter.currentGeneration(), generationBefore);

  // A read staged against the OLD generation is therefore admitted, and the
  // new bytes arrive under the SAME generation value -- two Live reads can
  // legitimately share a generation while differing in content, which is
  // exactly why observations are never collapsed by generation/range.
  OwnerContext owner;
  PayloadResolver payloads;
  auto context        = makeContext(payloads);
  const auto key      = adapter.stageRead(MediaRange{0, 4}, generationBefore);
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  ASSERT_TRUE(sink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *sink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "bbbb");

  const auto observation = adapter.observation(1);
  ASSERT_TRUE(observation.has_value());
  EXPECT_EQ(observation->sourceGeneration, generationBefore);
  EXPECT_EQ(observation->consistency, SourceConsistency::Live);
}

// -- stageAt: caller-chosen keys for the cell-store path --------------------

TEST_F(FileSourceAdapterTest, StageAtUsesCallerKeyAndRefusesBadKeys) {
  writeFile("keyed ops");
  FileSourceAdapter adapter(path_);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  // The minted spec cell's ref IS the staged key: freezeRequest refuses
  // OwnerCells with no arena incarnation, so the key must be a real cell.
  const auto staged =
      adapter.stageAt(77, FileOpSpec{.kind  = FileOpSpec::Kind::Read,
                                     .range = MediaRange{0, 4}});
  ASSERT_TRUE(staged);
  EXPECT_EQ(*staged, 77U);

  // Key 0 and an occupied key are both refused.
  EXPECT_FALSE(adapter.stageAt(0, FileOpSpec{}));
  EXPECT_FALSE(adapter.stageAt(77, FileOpSpec{.kind  = FileOpSpec::Kind::Read,
                                              .range = MediaRange{0, 1}}));

  // The caller-keyed entry drives the ordinary prepare/execute path.
  const auto prepared = adapter.prepare(requestFor(77), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};
  adapter.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  ASSERT_TRUE(sink.payloadLease.has_value());
  const auto bytes = readPayload(payloads, *sink.payloadLease);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(textOf(*bytes), "keye");
}

// -- cancel evidence: mid-read and mid-write exit outcomes ------------------

TEST_F(FileSourceAdapterTest, CancelDuringReadYieldsCancelledNotComplete) {
  // A sparse 256 MiB file: the staging vector's value-init plus one large
  // pread keeps execute() busy far longer than the stop request's lead time,
  // so the cancel lands while the transfer is in flight -- the mid-read
  // exit the audit asks evidence for. Sparse keeps the fixture cheap; the
  // read still moves real bytes through the payload channel.
  constexpr std::uint64_t size = 256ULL * 1024 * 1024;
  {
    const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(size)), 0);
    ::close(fd);
  }
  FileSourceAdapter adapter(path_);
  PayloadResolver payloads;
  std::stop_source stop;
  // maxBytes must cover the whole range or toRead clamps below it.
  BackendContext context{.stop     = stop.get_token(),
                         .payloads = payloads,
                         .budget   = BackendBudget{size}};

  const auto key = adapter.stageRead(MediaRange{0, size}, 0);
  OwnerContext owner;
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};

  std::thread worker(
      [&] { adapter.execute(BackendCommand{{1}, *prepared}, context, sink); });
  // 256 MiB cannot memset+pread in 5 ms on any supported host, so the stop
  // always lands mid-execute; whether it precedes the first loop check or
  // the in-flight pread only decides if the Cancelled carries the payload.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  stop.request_stop();
  worker.join();

  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<Cancelled>(sink.candidate->outcome));
  // A cancelled read records no observation: only a Complete read mints one.
  EXPECT_FALSE(adapter.observation(1).has_value());
  ASSERT_TRUE(sink.released);
  if (sink.payloadLease) {
    // When the pread had already run, the Cancelled retains exactly the
    // bytes it moved -- all zeros from the sparse file.
    const auto bytes = readPayload(payloads, *sink.payloadLease);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->size(), size);
    EXPECT_EQ(bytes->front(), std::byte{0});
    EXPECT_EQ(bytes->back(), std::byte{0});
  }
}

TEST_F(FileSourceAdapterTest, CancelDuringWriteReportsUncertainPartial) {
  writeFile("x");
  FileSourceAdapter adapter(path_, /*writable=*/true);
  PayloadResolver payloads;
  std::stop_source stop;
  BackendContext context{.stop     = stop.get_token(),
                         .payloads = payloads,
                         .budget   = BackendBudget{64ULL * 1024 * 1024}};

  // RLIMIT_FSIZE caps the file at 48 MiB: the first pwrite of the 64 MiB
  // staged write (the largest stageWrite accepts) returns a SHORT count
  // (the limit), the loop iterates, and the stop request -- landed while
  // that first pwrite is in flight -- is seen at the top of the second
  // iteration. That is the genuinely partial write the Cancelled.uncertain
  // flag exists for: bytes may have landed and the caller cannot tell how
  // many without revalidating.
  constexpr std::uint64_t fileLimit = 48ULL * 1024 * 1024;
  constexpr std::uint64_t writeSize = 64ULL * 1024 * 1024;
  struct rlimit oldLimit{};
  ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &oldLimit), 0);
  const struct rlimit newLimit{fileLimit, fileLimit};
  ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &newLimit), 0);
  const auto oldXfsz = ::signal(SIGXFSZ, SIG_IGN);

  std::vector<std::byte> payload(writeSize, std::byte{'w'});
  const auto key = adapter.stageWrite(0, std::move(payload), 0);
  ASSERT_NE(key, 0U);
  OwnerContext owner;
  const auto prepared = adapter.prepare(requestFor(key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink{&payloads};

  std::thread worker(
      [&] { adapter.execute(BackendCommand{{1}, *prepared}, context, sink); });
  // The first pwrite moves fileLimit bytes (~5-15 ms); stopping at 1 ms
  // lands inside it on any realistic host, so iteration two observes the
  // stop. Even a stop that beats the worker to the first check still
  // yields Cancelled -- only the transferred byte count differs.
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  stop.request_stop();
  worker.join();

  // Restore process state BEFORE asserting: a failed ASSERT must not leak
  // the file-size limit or the ignored signal into the rest of the suite.
  static_cast<void>(::setrlimit(RLIMIT_FSIZE, &oldLimit));
  static_cast<void>(::signal(SIGXFSZ, oldXfsz));

  ASSERT_TRUE(sink.candidate);
  const auto *cancelled = std::get_if<Cancelled>(&sink.candidate->outcome);
  ASSERT_NE(cancelled, nullptr);
  EXPECT_TRUE(cancelled->uncertain);
  // written is all-or-nothing per pwrite: either the stop beat the first
  // iteration (0) or the full RLIMIT-capped chunk landed (fileLimit).
  EXPECT_TRUE(cancelled->progress.transferredBytes == 0 ||
              cancelled->progress.transferredBytes == fileLimit);
  ASSERT_TRUE(sink.released);

  // The file never grew past the rlimit, whatever landed.
  struct stat st{};
  ASSERT_EQ(::stat(path_.c_str(), &st), 0);
  EXPECT_LE(static_cast<std::uint64_t>(st.st_size), fileLimit);
}

} // namespace

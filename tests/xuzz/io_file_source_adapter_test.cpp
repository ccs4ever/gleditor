#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <sstream>
#include <string>
#include <sys/stat.h>
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

} // namespace

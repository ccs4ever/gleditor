// M7-VQL-c C1: the write half of the file host -- grants, write modes, sync,
// and the service budget charged for a write's bytes. Language-free: the
// host is driven through its own staging calls, as a front end's runner
// does (design/vortex-io-m7-vql-c.md §4, §10, §18 C1).

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {
using namespace xanadu::vortex;
namespace fs = std::filesystem;

auto bytesOf(const std::string_view text) -> std::vector<std::byte> {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i)
    out[i] = static_cast<std::byte>(text[i]);
  return out;
}

auto slurp(const fs::path &path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// What a request ended as, read through the bridge's field accessor.
struct Ended {
  IoOutcomeTag tag{};
  std::optional<std::string> errorCode;
  std::optional<std::string> detail;
  std::optional<std::string> transferred;
};

/// A host over a fresh directory tree: `root/` is writable by grant,
/// `outside/` is not.
class FileWriteHostTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::uint32_t seq = 0;
    // Canonical already: the grant canonicalises, so a /tmp that is itself a
    // symlink must not make every root look like an escape.
    base_ = fs::weakly_canonical(fs::temp_directory_path()) /
            ("io_file_write_host_" + std::to_string(::getpid()) + "_" +
             std::to_string(seq++));
    root_    = base_ / "root";
    outside_ = base_ / "outside";
    fs::create_directories(root_);
    fs::create_directories(outside_);
  }
  void TearDown() override {
    host_.reset();
    std::error_code ec;
    fs::remove_all(base_, ec);
  }

  void startHost(FileReadHostOptions options = {}) {
    host_ = std::make_unique<FileReadHost>(vm_, arena_, std::move(options));
  }
  [[nodiscard]] static auto grantFor(const fs::path &root) -> WriteGrant {
    return writeRootGrant({root});
  }
  auto &host() { return *host_; }

  void spinUntil(const std::function<bool()> &done) {
    for (int i = 0; i < 4000 && !done(); ++i) {
      host().pump(16);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /// Admit an open and return either its capability or the refusing/ending
  /// outcome. The open is polled through the pump, never waited on.
  struct Opened {
    std::optional<CapabilityCell> capability;
    std::optional<Ended> ended;
  };
  auto open(const fs::path &path, const std::uint64_t rights) -> Opened {
    const auto submitted =
        host().host().admitOpen(host().openSpec(path.string(), rights));
    if (const auto *refused = std::get_if<Refused>(&submitted.value))
      return {std::nullopt, describe(refused->outcome)};
    const auto request = std::get<Admitted>(submitted.value).request;
    std::optional<std::expected<CapabilityCell, FrontendDiagnostic>> result;
    spinUntil([&] {
      result = host().openedCapability(request);
      return result.has_value();
    });
    if (result && *result) return {**result, std::nullopt};
    const auto polled = host().service().poll(request);
    if (polled.outcome) return {std::nullopt, describe(*polled.outcome)};
    return {};
  }

  auto describe(const OutcomeCell outcome) -> Ended {
    auto &bridge = host().bridge();
    return Ended{ioOutcomeTag(arena_, outcome).value(),
                 bridge.outcomeField(outcome, "errorCode"),
                 bridge.outcomeField(outcome, "stableDetail"),
                 bridge.outcomeField(outcome, "transferredBytes")};
  }

  /// Submit a staged spec and wait for its terminal outcome.
  auto finish(const CapabilityCell capability,
              const std::expected<zigzag::CellRef, StageReadError> &spec)
      -> std::optional<Ended> {
    EXPECT_TRUE(spec.has_value());
    if (!spec) return std::nullopt;
    const auto submitted = host().service().submit(capability, *spec);
    if (const auto *refused = std::get_if<Refused>(&submitted.value))
      return describe(refused->outcome);
    const auto request = std::get<Admitted>(submitted.value).request;
    std::optional<OutcomeCell> outcome;
    spinUntil([&] {
      outcome = host().service().poll(request).outcome;
      return outcome.has_value();
    });
    if (!outcome) return std::nullopt;
    return describe(*outcome);
  }

  zigzag::ArenaManifold arena_;
  zigzag::vortex::VortexCore core_{arena_};
  zigzag::vortex::VortexVM vm_{core_};
  std::unique_ptr<FileReadHost> host_;
  fs::path base_, root_, outside_;
};

constexpr std::uint64_t writeRights  = fileRightWrite;
constexpr std::uint64_t appendRights = fileRightAppend;
constexpr std::uint64_t createWrite  = fileRightWrite | fileRightCreate;
constexpr std::uint64_t createAppend = fileRightAppend | fileRightCreate;

// ---- authority -----------------------------------------------------------

TEST_F(FileWriteHostTest,
       WithNoGrantEveryWriteOpenIsRejectedAndNothingChanges) {
  startHost();
  std::ofstream(root_ / "keep.txt") << "keep";
  for (const auto rights :
       {writeRights, appendRights, createWrite, createAppend}) {
    const auto opened = open(root_ / "keep.txt", rights);
    EXPECT_FALSE(opened.capability.has_value());
    ASSERT_TRUE(opened.ended.has_value());
    EXPECT_EQ(opened.ended->tag, IoOutcomeTag::Rejected);
    EXPECT_EQ(opened.ended->errorCode, "Unsupported");
    EXPECT_EQ(opened.ended->detail, std::to_string(fileDetailNoWriteGrant));
  }
  const auto created = open(root_ / "new.txt", createWrite);
  ASSERT_TRUE(created.ended.has_value());
  EXPECT_EQ(created.ended->tag, IoOutcomeTag::Rejected);
  EXPECT_FALSE(fs::exists(root_ / "new.txt")) << "denied create made a file";
  EXPECT_EQ(slurp(root_ / "keep.txt"), "keep");
  EXPECT_EQ(host().adapter().openedCount(), 0U);
}

TEST_F(FileWriteHostTest, ReadOpensNeedNoGrant) {
  startHost();
  std::ofstream(root_ / "r.txt") << "readable";
  const auto opened = open(root_ / "r.txt", fileRightRead);
  EXPECT_TRUE(opened.capability.has_value());
}

TEST_F(FileWriteHostTest, AGrantAllowsAWriteInsideItsRootOnly) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  std::ofstream(root_ / "in.txt") << "0123456789";
  std::ofstream(outside_ / "out.txt") << "0123456789";

  const auto inside = open(root_ / "in.txt", writeRights);
  ASSERT_TRUE(inside.capability.has_value());
  const auto ended =
      finish(*inside.capability,
             host().stageWrite(*inside.capability, 2, bytesOf("XY")));
  ASSERT_TRUE(ended.has_value());
  EXPECT_EQ(ended->tag, IoOutcomeTag::Complete);
  EXPECT_EQ(ended->transferred, "2");
  EXPECT_EQ(slurp(root_ / "in.txt"), "01XY456789");

  const auto elsewhere = open(outside_ / "out.txt", writeRights);
  ASSERT_TRUE(elsewhere.ended.has_value());
  EXPECT_EQ(elsewhere.ended->tag, IoOutcomeTag::Rejected);
  EXPECT_EQ(slurp(outside_ / "out.txt"), "0123456789");
}

TEST_F(FileWriteHostTest, DotDotCannotEscapeTheRoot) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  std::ofstream(outside_ / "out.txt") << "safe";
  fs::create_directories(root_ / "sub");

  // Existing prefix, then up and out; and a missing component that a lexical
  // check would "cancel" against the following `..`.
  for (const auto &spelling :
       {root_ / "sub" / ".." / ".." / "outside" / "out.txt",
        root_ / "missing" / ".." / ".." / "outside" / "out.txt"}) {
    const auto opened = open(spelling, writeRights);
    EXPECT_FALSE(opened.capability.has_value()) << spelling;
    ASSERT_TRUE(opened.ended.has_value()) << spelling;
    EXPECT_EQ(opened.ended->tag, IoOutcomeTag::Rejected) << spelling;
  }
  // `..` that stays inside the root is still inside.
  const auto inside = open(root_ / "sub" / ".." / "ok.txt", createWrite);
  EXPECT_TRUE(inside.capability.has_value());
  EXPECT_TRUE(fs::exists(root_ / "ok.txt"));
  EXPECT_EQ(slurp(outside_ / "out.txt"), "safe");
}

TEST_F(FileWriteHostTest, ASymlinkedDirectoryCannotEscapeTheRoot) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  std::ofstream(outside_ / "out.txt") << "safe";
  fs::create_directory_symlink(outside_, root_ / "linkdir");
  fs::create_symlink(outside_ / "out.txt", root_ / "linkfile");

  for (const auto &spelling :
       {root_ / "linkdir" / "out.txt", root_ / "linkdir" / "fresh.txt",
        root_ / "linkfile"}) {
    const auto opened = open(spelling, createWrite);
    EXPECT_FALSE(opened.capability.has_value()) << spelling;
    ASSERT_TRUE(opened.ended.has_value()) << spelling;
    EXPECT_EQ(opened.ended->tag, IoOutcomeTag::Rejected) << spelling;
  }
  const auto write = open(root_ / "linkdir" / "out.txt", writeRights);
  EXPECT_FALSE(write.capability.has_value());
  EXPECT_FALSE(fs::exists(outside_ / "fresh.txt"));
  EXPECT_EQ(slurp(outside_ / "out.txt"), "safe");
}

TEST_F(FileWriteHostTest, ASymlinkedRootNamesItsTarget) {
  fs::create_directory_symlink(root_, base_ / "rootlink");
  FileReadHostOptions options;
  options.writeGrant = grantFor(base_ / "rootlink");
  startHost(options);
  EXPECT_TRUE(open(root_ / "a.txt", createWrite).capability.has_value());
  EXPECT_FALSE(open(outside_ / "a.txt", createWrite).capability.has_value());
}

TEST_F(FileWriteHostTest, ASiblingWhoseNameStartsWithTheRootIsOutside) {
  fs::create_directories(base_ / "rootlike");
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  EXPECT_FALSE(
      open(base_ / "rootlike" / "a.txt", createWrite).capability.has_value());
}

TEST_F(FileWriteHostTest, TheRootItselfIsNotAWritableFile) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  EXPECT_FALSE(open(root_, writeRights).capability.has_value());
}

TEST_F(FileWriteHostTest, ALocatorWithAnEmbeddedNulIsNeverGranted) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  // The OS would open only the prefix before the NUL, a path the grant never
  // saw.
  OpenSpec spec        = host().openSpec((root_ / "x").string(), writeRights);
  spec.locator         = bytesOf((root_ / "x").string() + std::string(1, '\0') +
                                 "/../../outside/y");
  const auto submitted = host().host().admitOpen(spec);
  const auto *refused  = std::get_if<Refused>(&submitted.value);
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(describe(refused->outcome).tag, IoOutcomeTag::Rejected);
}

TEST_F(FileWriteHostTest, AGrantThatApprovesLessThanAskedDeniesTheWholeOpen) {
  FileReadHostOptions options;
  options.writeGrant = writeRootGrant({root_}, fileRightWrite);
  startHost(options);
  std::ofstream(root_ / "a.txt") << "a";
  EXPECT_TRUE(open(root_ / "a.txt", writeRights).capability.has_value());
  const auto create = open(root_ / "b.txt", createWrite);
  EXPECT_FALSE(create.capability.has_value());
  EXPECT_FALSE(fs::exists(root_ / "b.txt"));
  EXPECT_FALSE(open(root_ / "a.txt", appendRights).capability.has_value());
}

// ---- create, write, append, sync -----------------------------------------

TEST_F(FileWriteHostTest, CreateIsExclusiveAndNeverClobbers) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);

  const auto first = open(root_ / "new.txt", createWrite);
  ASSERT_TRUE(first.capability.has_value());
  EXPECT_TRUE(fs::exists(root_ / "new.txt"));
  ASSERT_TRUE(finish(*first.capability,
                     host().stageWrite(*first.capability, 0, bytesOf("v1")))
                  .has_value());
  EXPECT_EQ(slurp(root_ / "new.txt"), "v1");
  EXPECT_EQ(fs::status(root_ / "new.txt").permissions() &
                (fs::perms::group_all | fs::perms::others_all),
            fs::perms::none)
      << "a created file starts private";

  const auto second = open(root_ / "new.txt", createWrite);
  EXPECT_FALSE(second.capability.has_value());
  ASSERT_TRUE(second.ended.has_value());
  EXPECT_EQ(second.ended->tag, IoOutcomeTag::Failed);
  EXPECT_EQ(second.ended->errorCode, "BackendFailure");
  EXPECT_EQ(second.ended->detail, std::to_string(EEXIST));
  EXPECT_EQ(slurp(root_ / "new.txt"), "v1");
}

TEST_F(FileWriteHostTest, OpeningAMissingFileWithoutCreateFails) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  const auto opened = open(root_ / "absent.txt", writeRights);
  ASSERT_TRUE(opened.ended.has_value());
  EXPECT_EQ(opened.ended->tag, IoOutcomeTag::Failed);
  EXPECT_FALSE(fs::exists(root_ / "absent.txt"));
}

TEST_F(FileWriteHostTest, WriteAndAppendTogetherOrCreateAloneAreRefused) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  for (const auto rights :
       {fileRightWrite | fileRightAppend, fileRightCreate}) {
    const auto opened = open(root_ / "x.txt", rights);
    EXPECT_FALSE(opened.capability.has_value());
    ASSERT_TRUE(opened.ended.has_value());
    EXPECT_EQ(opened.ended->tag, IoOutcomeTag::Rejected);
    EXPECT_EQ(opened.ended->detail, std::to_string(fileDetailWrongMode));
  }
  EXPECT_FALSE(fs::exists(root_ / "x.txt"));
}

TEST_F(FileWriteHostTest, AppendsLandInOrderAtTheEnd) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  std::ofstream(root_ / "log.txt") << "start:";

  const auto opened = open(root_ / "log.txt", appendRights);
  ASSERT_TRUE(opened.capability.has_value());
  for (const char *line : {"a;", "b;", "c;"}) {
    const auto ended =
        finish(*opened.capability,
               host().stageAppend(*opened.capability, bytesOf(line)));
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->tag, IoOutcomeTag::Complete);
    EXPECT_EQ(ended->transferred, "2");
  }
  EXPECT_EQ(slurp(root_ / "log.txt"), "start:a;b;c;");
}

TEST_F(FileWriteHostTest, AppendWithCreateStartsAnEmptyFile) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  const auto opened = open(root_ / "fresh.log", createAppend);
  ASSERT_TRUE(opened.capability.has_value());
  ASSERT_TRUE(finish(*opened.capability,
                     host().stageAppend(*opened.capability, bytesOf("x")))
                  .has_value());
  EXPECT_EQ(slurp(root_ / "fresh.log"), "x");
}

TEST_F(FileWriteHostTest, TheWrongWriteDisciplineForACapabilityIsRejected) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  std::ofstream(root_ / "f.txt") << "0123";

  const auto positional = open(root_ / "f.txt", writeRights);
  const auto appending  = open(root_ / "f.txt", appendRights);
  const auto readOnly   = open(root_ / "f.txt", fileRightRead);
  ASSERT_TRUE(positional.capability && appending.capability &&
              readOnly.capability);

  const auto check = [&](const std::optional<Ended> &ended) {
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->tag, IoOutcomeTag::Rejected);
    EXPECT_EQ(ended->errorCode, "Unsupported");
    EXPECT_EQ(ended->detail, std::to_string(fileDetailWrongMode));
  };
  check(finish(*positional.capability,
               host().stageAppend(*positional.capability, bytesOf("x"))));
  check(finish(*appending.capability,
               host().stageWrite(*appending.capability, 0, bytesOf("x"))));
  check(finish(*readOnly.capability,
               host().stageWrite(*readOnly.capability, 0, bytesOf("x"))));
  check(finish(*readOnly.capability,
               host().stageAppend(*readOnly.capability, bytesOf("x"))));
  check(finish(*readOnly.capability, host().stageSync(*readOnly.capability)));
  EXPECT_EQ(slurp(root_ / "f.txt"), "0123");
}

TEST_F(FileWriteHostTest,
       AWriteLargerThanTheCompletionEnvelopeReportsEveryByte) {
  // The backend's per-request byte bound is sized from the completion
  // envelope (a few hundred bytes for a write); a write's own bytes are
  // governed by the command budget, so a full-size write must not trip it.
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  const auto opened = open(root_ / "big.bin", createWrite);
  ASSERT_TRUE(opened.capability.has_value());
  const auto size = FileReadHostOptions::defaultWriteBytes;
  const auto ended =
      finish(*opened.capability,
             host().stageWrite(*opened.capability, 0,
                               std::vector<std::byte>(size, std::byte{'q'})));
  ASSERT_TRUE(ended.has_value());
  EXPECT_EQ(ended->tag, IoOutcomeTag::Complete);
  EXPECT_EQ(ended->transferred, std::to_string(size));
  EXPECT_EQ(fs::file_size(root_ / "big.bin"), size);
}

TEST_F(FileWriteHostTest, ZeroByteWriteAndAppendComplete) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  std::ofstream(root_ / "f.txt") << "same";
  const auto w = open(root_ / "f.txt", writeRights);
  const auto a = open(root_ / "f.txt", appendRights);
  ASSERT_TRUE(w.capability && a.capability);
  const auto wrote =
      finish(*w.capability, host().stageWrite(*w.capability, 0, {}));
  const auto added =
      finish(*a.capability, host().stageAppend(*a.capability, {}));
  ASSERT_TRUE(wrote && added);
  EXPECT_EQ(wrote->tag, IoOutcomeTag::Complete);
  EXPECT_EQ(added->tag, IoOutcomeTag::Complete);
  EXPECT_EQ(wrote->transferred, "0");
  EXPECT_EQ(slurp(root_ / "f.txt"), "same");
}

TEST_F(FileWriteHostTest, SyncCompletesAfterTheWritesAdmittedBeforeIt) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  startHost(options);
  const auto opened = open(root_ / "d.txt", createWrite);
  ASSERT_TRUE(opened.capability.has_value());
  const auto cap = *opened.capability;

  // Two writes then a sync, all admitted before anything is awaited: the
  // write lane runs them in admission order, so the later write wins the
  // overlap and the sync cannot finish before either write.
  const auto submit =
      [&](const std::expected<zigzag::CellRef, StageReadError> &spec) {
        EXPECT_TRUE(spec.has_value());
        const auto submitted = host().service().submit(cap, *spec);
        EXPECT_TRUE(std::holds_alternative<Admitted>(submitted.value));
        return std::get<Admitted>(submitted.value).request;
      };
  const auto big   = submit(host().stageWrite(
      cap, 0,
      std::vector<std::byte>(FileReadHostOptions::defaultWriteBytes,
                               std::byte{'A'})));
  const auto small = submit(host().stageWrite(cap, 0, bytesOf("BB")));
  const auto sync  = submit(host().stageSync(cap));

  std::optional<OutcomeCell> syncOutcome;
  spinUntil([&] {
    syncOutcome = host().service().poll(sync).outcome;
    return syncOutcome.has_value();
  });
  ASSERT_TRUE(syncOutcome.has_value());
  EXPECT_EQ(describe(*syncOutcome).tag, IoOutcomeTag::Complete);
  EXPECT_TRUE(host().service().poll(big).outcome.has_value())
      << "sync finished before an earlier write";
  EXPECT_TRUE(host().service().poll(small).outcome.has_value());
  const auto content = slurp(root_ / "d.txt");
  ASSERT_EQ(content.size(), FileReadHostOptions::defaultWriteBytes);
  EXPECT_EQ(content.substr(0, 3), "BBA");
}

// ---- bounds and budget ---------------------------------------------------

TEST_F(FileWriteHostTest, AWriteOverTheWriteBoundIsRefusedBeforeStaging) {
  FileReadHostOptions options;
  options.writeGrant = grantFor(root_);
  options.writeBytes = 16;
  startHost(options);
  std::ofstream(root_ / "f.txt") << "0123";
  const auto opened = open(root_ / "f.txt", writeRights);
  ASSERT_TRUE(opened.capability.has_value());

  const auto over =
      host().stageWrite(*opened.capability, 0, std::vector<std::byte>(17));
  ASSERT_FALSE(over.has_value());
  EXPECT_EQ(over.error(), StageReadError::OutOfBounds);
  const auto overAppend =
      host().stageAppend(*opened.capability, std::vector<std::byte>(17));
  EXPECT_EQ(overAppend.error(), StageReadError::OutOfBounds);
  const auto wraps = host().stageWrite(
      *opened.capability, std::numeric_limits<std::uint64_t>::max(),
      bytesOf("x"));
  EXPECT_EQ(wraps.error(), StageReadError::OutOfBounds);
  // At the bound it is accepted.
  EXPECT_TRUE(
      finish(*opened.capability,
             host().stageWrite(*opened.capability, 0,
                               std::vector<std::byte>(16, std::byte{'z'})))
          .has_value());
  EXPECT_EQ(slurp(root_ / "f.txt"), std::string(16, 'z'));
}

TEST_F(FileWriteHostTest, WriteBytesAreChargedToTheServiceCommandBudget) {
  FileReadHostOptions options;
  options.writeGrant  = grantFor(root_);
  options.budgetBytes = 64U * 1024U;
  startHost(options);
  const auto opened = open(root_ / "f.txt", createWrite);
  ASSERT_TRUE(opened.capability.has_value());

  // 100 KiB passes the 1 MiB write bound but not the service's 64 KiB command
  // budget. Before C1 the command was 8 bytes, so this was admitted.
  const auto ended =
      finish(*opened.capability,
             host().stageWrite(*opened.capability, 0,
                               std::vector<std::byte>(100U * 1024U)));
  ASSERT_TRUE(ended.has_value());
  EXPECT_EQ(ended->tag, IoOutcomeTag::Rejected);
  EXPECT_EQ(ended->errorCode, "BudgetExceeded");
  EXPECT_EQ(fs::file_size(root_ / "f.txt"), 0U) << "a refused write wrote";
}

TEST_F(FileWriteHostTest, InFlightWritesShareOneBudgetAndItComesBack) {
  FileReadHostOptions options;
  options.writeGrant  = grantFor(root_);
  options.budgetBytes = 64U * 1024U;
  startHost(options);
  const auto opened = open(root_ / "f.txt", createWrite);
  ASSERT_TRUE(opened.capability.has_value());
  const auto cap = *opened.capability;

  // Nothing is pumped, so no request is reaped and each keeps its reserved
  // bytes: two 30 KiB writes fit in 64 KiB, the third does not.
  std::vector<RequestCell> admitted;
  std::optional<Ended> refused;
  for (int i = 0; i < 3; ++i) {
    const auto spec =
        host().stageWrite(cap, 0, std::vector<std::byte>(30U * 1024U));
    ASSERT_TRUE(spec.has_value());
    const auto submitted = host().service().submit(cap, *spec);
    if (const auto *ok = std::get_if<Admitted>(&submitted.value)) {
      admitted.push_back(ok->request);
    } else {
      refused = describe(std::get<Refused>(submitted.value).outcome);
    }
  }
  EXPECT_EQ(admitted.size(), 2U);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->tag, IoOutcomeTag::Rejected);
  EXPECT_EQ(refused->errorCode, "BudgetExceeded");

  // Settled and retired, the reservation returns and a write fits again.
  spinUntil([&] {
    for (const auto request : admitted)
      if (!host().service().poll(request).outcome) return false;
    return true;
  });
  for (const auto request : admitted) host().service().retireRequest(request);
  const auto again = finish(
      cap, host().stageWrite(cap, 0, std::vector<std::byte>(30U * 1024U)));
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->tag, IoOutcomeTag::Complete);
}

} // namespace

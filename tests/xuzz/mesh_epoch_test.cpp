#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <set>
#include <vector>

#include "common/xanadu/provenance.hpp"
#include "common/xanadu/vortex/mesh_epoch.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace zigzag::vortex::mesh {
namespace {

class MeshEpochTest : public testing::Test {
protected:
  xanadu::identity::Fingerprint author =
      *xanadu::identity::Fingerprint::fromString(std::string(40, 'A'));
  xanadu::PublicKey device;
  std::filesystem::path directory;

  MeshEpochTest() {
    static std::atomic<unsigned> counter{0};
    device.bytes[0] = 1;
    directory =
        std::filesystem::temp_directory_path() /
        ("xanadu-mesh-epoch-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(counter++));
    std::filesystem::create_directories(directory);
  }
  ~MeshEpochTest() override { std::filesystem::remove_all(directory); }
  std::filesystem::path statePath() const {
    return directory / (device.hex() + ".epochs");
  }
  std::string header() const {
    return "XANADU-MESH-EPOCH 1 " + author.toString() + " " + device.hex() +
           "\n";
  }
  void writeState(std::string_view bytes) const {
    std::ofstream out(statePath(), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("could not write test epoch journal");
  }
  std::string readState() const {
    std::ifstream in(statePath(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }
};

TEST_F(MeshEpochTest, InvalidIdentityRefusesBeforeCreatingState) {
  auto invalid   = author;
  invalid.hex[0] = 'Z';
  EXPECT_FALSE(reserveNodeId(directory, invalid, device));
  EXPECT_FALSE(reserveNodeId(directory, author, xanadu::PublicKey{}));
  EXPECT_FALSE(
      reserveNodeId(directory, xanadu::identity::Fingerprint{}, device));
  EXPECT_TRUE(std::filesystem::is_empty(directory));
}

#if defined(__unix__) || defined(__APPLE__)
TEST_F(MeshEpochTest, ReopeningReservesDurableMonotonicIncarnations) {
  for (std::uint64_t expected = 1; expected <= 8; ++expected) {
    auto result = reserveNodeId(directory, author, device);
    ASSERT_TRUE(result) << result.error();
    EXPECT_EQ(result->authorKey, author.toString());
    EXPECT_EQ(result->deviceKey, device.hex());
    EXPECT_EQ(result->arenaEpoch, expected);
  }
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                          std::filesystem::directory_iterator()),
            2);
}

TEST_F(MeshEpochTest, IndependentDevicesAndPinnedAuthor) {
  auto first = reserveNodeId(directory, author, device);
  ASSERT_TRUE(first) << first.error();
  auto otherDevice     = device;
  otherDevice.bytes[1] = 2;
  auto other           = reserveNodeId(directory, author, otherDevice);
  ASSERT_TRUE(other) << other.error();
  EXPECT_EQ(other->arenaEpoch, 1);
  auto otherAuthor =
      *xanadu::identity::Fingerprint::fromString(std::string(40, 'B'));
  EXPECT_FALSE(reserveNodeId(directory, otherAuthor, device));
  auto again = reserveNodeId(directory, author, device);
  ASSERT_TRUE(again) << again.error();
  EXPECT_EQ(again->arenaEpoch, 2);
}

TEST_F(MeshEpochTest, ConcurrentThreadsReceiveDistinctReservations) {
  std::vector<std::future<std::expected<NodeId, std::string>>> tasks;
  for (unsigned i = 0; i < 12; ++i)
    tasks.push_back(std::async(std::launch::async, [this] {
      return reserveNodeId(directory, author, device);
    }));
  std::set<std::uint64_t> epochs;
  for (auto &task : tasks) {
    auto result = task.get();
    ASSERT_TRUE(result) << result.error();
    epochs.insert(result->arenaEpoch);
  }
  EXPECT_EQ(epochs.size(), 12);
  EXPECT_EQ(*epochs.begin(), 1);
  EXPECT_EQ(*epochs.rbegin(), 12);
}

TEST_F(MeshEpochTest, ConcurrentProcessesSerializeOnTheDeviceJournal) {
  auto initial = reserveNodeId(directory, author, device);
  ASSERT_TRUE(initial) << initial.error();
  std::vector<pid_t> children;
  for (unsigned i = 0; i < 8; ++i) {
    auto pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      auto result = reserveNodeId(directory, author, device);
      ::_exit(result ? static_cast<int>(result->arenaEpoch) : 0);
    }
    children.push_back(pid);
  }
  std::set<int> epochs;
  for (auto child : children) {
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_GE(WEXITSTATUS(status), 2);
    EXPECT_LE(WEXITSTATUS(status), 9);
    epochs.insert(WEXITSTATUS(status));
  }
  EXPECT_EQ(epochs.size(), 8);
  auto final = reserveNodeId(directory, author, device);
  ASSERT_TRUE(final) << final.error();
  EXPECT_EQ(final->arenaEpoch, 10);
}

TEST_F(MeshEpochTest, InterruptedOrCorruptStateIsNeverReset) {
  auto initial = reserveNodeId(directory, author, device);
  ASSERT_TRUE(initial) << initial.error();
  const auto valid              = readState();
  const std::string badStates[] = {"",
                                   header(),
                                   valid.substr(0, valid.size() - 1),
                                   valid + "2 ",
                                   "XANADU-MESH-EPOCH 2\n",
                                   header() + "01 bad\n",
                                   header() + "1 bad\n",
                                   valid + std::string(257, 'x') + "\n"};
  for (const auto &bad : badStates) {
    writeState(bad);
    auto result = reserveNodeId(directory, author, device);
    EXPECT_FALSE(result);
    EXPECT_EQ(readState(), bad);
    EXPECT_FALSE(reserveNodeId(directory, author, device));
  }
}

TEST_F(MeshEpochTest, ExhaustedCounterRefusesWithoutWriting) {
  auto number = std::to_string(std::numeric_limits<std::uint64_t>::max());
  auto record =
      header() + number + " " + xanadu::sha256Hex(header() + number) + "\n";
  writeState(record);
  auto result = reserveNodeId(directory, author, device);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("exhausted"), std::string::npos);
  EXPECT_EQ(readState(), record);
}

TEST_F(MeshEpochTest, MissingDirectoryAndSymlinkStateRefuse) {
  auto missing = directory / "missing";
  EXPECT_FALSE(reserveNodeId(missing, author, device));
  EXPECT_FALSE(std::filesystem::exists(missing));
  auto target = directory / "unrelated";
  {
    std::ofstream out(target);
    out << "untouched";
  }
  std::filesystem::create_symlink(target, statePath());
  EXPECT_FALSE(reserveNodeId(directory, author, device));
  std::ifstream in(target);
  std::string text;
  in >> text;
  EXPECT_EQ(text, "untouched");
}
#else
TEST_F(MeshEpochTest, UnsupportedPlatformRefusesExplicitly) {
  auto result = reserveNodeId(directory, author, device);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("unsupported"), std::string::npos);
}
#endif
} // namespace
} // namespace zigzag::vortex::mesh

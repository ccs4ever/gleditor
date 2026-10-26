#include "common/xanadu/vortex/mesh_epoch.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

#include "common/xanadu/provenance.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace zigzag::vortex::mesh {
namespace {
#if defined(__unix__) || defined(__APPLE__)
class File {
public:
  explicit File(int fd) : fd_(fd) {
    if (fd < 0) fail("open");
  }
  File(const File &)            = delete;
  File &operator=(const File &) = delete;
  ~File() { ::close(fd_); }
  [[nodiscard]] int get() const noexcept { return fd_; }
  [[noreturn]] static void fail(std::string_view operation) {
    throw std::runtime_error("mesh epoch " + std::string(operation) + ": " +
                             std::strerror(errno));
  }

private:
  int fd_;
};

void regularFile(const File &file) {
  struct stat status{};
  if (::fstat(file.get(), &status) != 0) File::fail("stat");
  if (!S_ISREG(status.st_mode))
    throw std::runtime_error("mesh epoch state is not a regular file");
}
void syncFile(const File &file) {
  while (::fsync(file.get()) != 0)
    if (errno != EINTR) File::fail("fsync");
}
void writeAll(const File &file, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto written = ::write(file.get(), bytes.data(), bytes.size());
    if (written < 0) {
      if (errno == EINTR) continue;
      File::fail("write");
    }
    if (written == 0)
      throw std::runtime_error("mesh epoch write made no progress");
    bytes.remove_prefix(static_cast<std::size_t>(written));
  }
}

// Each complete record authenticates the header and its predecessor. A crash
// can leave a complete new reservation or an invalid tail, never a valid lower
// counter produced by an in-place partially written decimal replacement.
std::uint64_t readEpoch(const File &file, const std::string &header,
                        std::string &previousHash) {
  std::string line;
  bool headerSeen     = false;
  bool recordSeen     = false;
  std::uint64_t epoch = 0;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(file.get(), buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) continue;
      File::fail("read");
    }
    if (count == 0) break;
    for (std::ptrdiff_t i = 0; i < count; ++i) {
      if (buffer[i] != '\n') {
        if (line.size() >= 256)
          throw std::runtime_error("mesh epoch journal record is oversized");
        line.push_back(buffer[i]);
        continue;
      }
      if (!headerSeen) {
        if (line + '\n' != header)
          throw std::runtime_error(
              "mesh epoch journal identity or version mismatch");
        headerSeen = true;
      } else {
        const auto separator = line.find(' ');
        if (separator == std::string::npos)
          throw std::runtime_error("mesh epoch journal has a malformed record");
        const std::string_view number(line.data(), separator);
        std::uint64_t next{};
        auto parsed =
            std::from_chars(number.data(), number.data() + number.size(), next);
        const auto hash =
            xanadu::sha256Hex(header + previousHash + std::string(number));
        if (parsed.ec != std::errc{} ||
            parsed.ptr != number.data() + number.size() ||
            std::to_string(next) != number || next <= epoch ||
            line.substr(separator + 1) != hash)
          throw std::runtime_error(
              "mesh epoch journal counter or checksum is invalid");
        epoch        = next;
        previousHash = hash;
        recordSeen   = true;
      }
      line.clear();
    }
  }
  if (!line.empty() || !headerSeen || !recordSeen)
    throw std::runtime_error("mesh epoch journal is empty or interrupted");
  return epoch;
}
#endif
} // namespace

std::expected<NodeId, std::string>
reserveNodeId(const std::filesystem::path &deploymentDirectory,
              const xanadu::identity::Fingerprint &author,
              const xanadu::PublicKey &device) {
  if (!author.isValid() || author.toString() == std::string(40, '0') ||
      device.isZero())
    return std::unexpected(
        "mesh epoch requires a valid author fingerprint and device public key");
#if defined(__unix__) || defined(__APPLE__)
  try {
    const File directory(
        ::open(deploymentDirectory.c_str(),
               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    const auto deviceName = device.hex();
    const auto lockName   = deviceName + ".epoch-lock";
    const File lock(
        ::openat(directory.get(), lockName.c_str(),
                 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    regularFile(lock);
    while (::flock(lock.get(), LOCK_EX) != 0)
      if (errno != EINTR) File::fail("lock");

    const auto stateName = deviceName + ".epochs";
    int fd               = ::openat(directory.get(), stateName.c_str(),
                                    O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC |
                                        O_NOFOLLOW | O_NONBLOCK,
                                    0600);
    const bool created   = fd >= 0;
    if (fd < 0 && errno == EEXIST)
      fd = ::openat(directory.get(), stateName.c_str(),
                    O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    const File state(fd);
    regularFile(state);
    const auto header =
        "XANADU-MESH-EPOCH 1 " + author.toString() + " " + deviceName + "\n";
    std::string previousHash;
    const auto previous = created ? 0 : readEpoch(state, header, previousHash);
    if (previous == std::numeric_limits<std::uint64_t>::max())
      return std::unexpected("mesh epoch counter exhausted");
    const auto next   = previous + 1;
    const auto number = std::to_string(next);
    const auto record =
        number + " " + xanadu::sha256Hex(header + previousHash + number) + "\n";
    writeAll(state, created ? header + record : record);
    syncFile(state);
    syncFile(directory);
    return NodeId{author.toString(), deviceName, next};
  } catch (const std::exception &error) {
    return std::unexpected(std::string(error.what()));
  }
#else
  (void)deploymentDirectory;
  return std::unexpected(
      "durable mesh epoch reservation is unsupported on this platform");
#endif
}
} // namespace zigzag::vortex::mesh

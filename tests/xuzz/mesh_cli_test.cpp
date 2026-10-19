#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/swarm.hpp"
#include "common/xanadu/user_permascroll.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace zigzag::vortex::mesh {
namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using V      = xanadu::bencode::Value;
std::string readFile(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
void writeFile(const fs::path &path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << contents;
  if (!out)
    throw std::runtime_error("mesh CLI fixture file could not be written");
}
#if defined(__unix__) || defined(__APPLE__)
class Child {
public:
  Child(const fs::path &program, std::vector<std::string> arguments,
        const fs::path &profile, fs::path output)
      : output_(std::move(output)) {
    fs::create_directories(profile);
    fs::create_directories(output_.parent_path());
    const auto executable = fs::absolute(program).string();
    arguments.insert(arguments.begin(), executable);
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto data   = (profile / "data").string(),
               config = (profile / "config").string(),
               cache  = (profile / "cache").string();
    const auto log    = output_.string();
    process_          = ::fork();
    if (process_ == -1)
      throw std::runtime_error("mesh CLI fixture fork failed");
    if (process_ == 0) {
      const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0) ::_exit(126);
      ::dup2(fd, STDOUT_FILENO);
      ::dup2(fd, STDERR_FILENO);
      ::close(fd);
      ::setenv("SDL_VIDEODRIVER", "offscreen", 1);
      ::setenv("SDL_AUDIODRIVER", "dummy", 1);
      ::setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
      ::setenv("XDG_DATA_HOME", data.c_str(), 1);
      ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
      ::setenv("XDG_CACHE_HOME", cache.c_str(), 1);
      ::execv(executable.c_str(), argv.data());
      ::_exit(127);
    }
  }
  ~Child() {
    if (process_ > 0) {
      ::kill(process_, SIGTERM);
      int status{};
      while (::waitpid(process_, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  Child(const Child &)            = delete;
  Child &operator=(const Child &) = delete;
  std::optional<int> wait(std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status{};
      const auto done = ::waitpid(process_, &status, WNOHANG);
      if (done == process_) {
        process_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      }
      if (done < 0 && errno != EINTR)
        throw std::runtime_error("mesh CLI fixture wait failed");
      std::this_thread::sleep_for(5ms);
    }
    return {};
  }
  std::string output() const { return readFile(output_); }

private:
  pid_t process_{};
  fs::path output_;
};
std::uint16_t freePort() {
  const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("mesh CLI fixture socket failed");
  sockaddr_in address{};
  address.sin_family      = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t size          = sizeof(address);
  const auto bound =
      ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  const auto named =
      bound == 0
          ? ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size)
          : -1;
  ::close(fd);
  if (named != 0)
    throw std::runtime_error("mesh CLI fixture port reservation failed");
  return ntohs(address.sin_port);
}
fs::path binary(std::string_view name) {
  return fs::current_path() / "build" / name;
}
class MeshCliTest : public testing::Test {
protected:
  fs::path root = fs::temp_directory_path() /
                  ("vmc-" + std::to_string(::getpid()) + "-" +
                   xanadu::createMutableKeys().publicKey.hex().substr(0, 8));
  ~MeshCliTest() override { fs::remove_all(root); }
};

TEST_F(
    MeshCliTest,
    EveryFrontendExposesMeshHelpAndRefusesMalformedDeploymentWithoutStartingWork) {
  const auto configuration = root / "invalid.bencode";
  writeFile(configuration, "not a deployment");
  for (const auto name : {"vpl", "vquery", "vprolog", "vlace"}) {
    SCOPED_TRACE(name);
    ASSERT_TRUE(fs::exists(binary(name)))
        << "Build all four frontends before running mesh CLI tests";
    Child help(binary(name), {"--mesh-help"}, root / name,
               root / (std::string(name) + "-help.log"));
    auto helped = help.wait(10s);
    ASSERT_TRUE(helped) << help.output();
    EXPECT_EQ(*helped, 0) << help.output();
    EXPECT_NE(help.output().find("--mesh-worker CONFIG SECONDS"),
              std::string::npos);
    EXPECT_NE(
        help.output().find("--mesh-run CONFIG AUTHOR:DEVICE:EPOCH SOURCE"),
        std::string::npos);
    Child invalid(binary(name), {"--mesh-worker", configuration.string(), "1"},
                  root / name, root / (std::string(name) + "-invalid.log"));
    auto refused = invalid.wait(10s);
    ASSERT_TRUE(refused) << invalid.output();
    EXPECT_NE(*refused, 0);
    EXPECT_EQ(invalid.output().find("mesh-node "), std::string::npos);
    Child unsupported(binary(name), {"--mesh-unknown"}, root / name,
                      root / (std::string(name) + "-unknown.log"));
    auto unhandled = unsupported.wait(10s);
    ASSERT_TRUE(unhandled) << unsupported.output();
    EXPECT_NE(*unhandled, 0);
    EXPECT_NE(unsupported.output().find("--mesh-help"), std::string::npos);
  }
}

struct ComputeIdentity {
  xanadu::MutableKeys keys;
  xanadu::DeviceDelegation delegation;
  std::string master;
};
ComputeIdentity makeIdentity(std::string name) {
  auto master = xanadu::identity::StandardCryptoEngine::generate();
  if (!master)
    throw std::runtime_error("mesh CLI fixture master generation failed");
  ComputeIdentity result;
  result.keys = xanadu::createMutableKeys();
  result.delegation.masterFingerprint =
      *xanadu::identity::Fingerprint::fromString(
          xanadu::identity::StandardCryptoEngine::computeFingerprint(
              master->publicKey()));
  result.delegation.devicePublicKey = result.keys.publicKey;
  result.delegation.deviceName      = std::move(name);
  const auto now                    = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  result.delegation.issuedTimestamp = now;
  xanadu::identity::CertificateConstraints constraints;
  constraints.serialNumber = 1;
  constraints.notBefore    = now - 1;
  constraints.notAfter     = now + 86400;
  constraints.deviceName   = result.delegation.deviceName;
  constraints.deviceId     = result.delegation.deviceName;
  constraints.authorName   = "Mesh CLI fixture";
  constraints.authorEmail  = "mesh@test.invalid";
  xanadu::identity::PubKey32 device;
  device.bytes = result.keys.publicKey.bytes;
  auto certificate =
      master->createDelegationCertificate(*master, device, constraints);
  if (!certificate)
    throw std::runtime_error("mesh CLI fixture device delegation failed");
  result.delegation.certificate = std::move(*certificate);
  xanadu::PublicKey publicMaster;
  publicMaster.bytes = master->publicKey().bytes;
  result.master      = publicMaster.hex();
  if (!result.delegation.verify(master->publicKey()))
    throw std::runtime_error("mesh CLI fixture delegation failed verification");
  return result;
}

TEST_F(
    MeshCliTest,
    SeparateRequesterProcessesUseEveryFrontendAgainstARealWorkerAndGpgSigner) {
  ASSERT_TRUE(fs::exists("/usr/bin/gpg"))
      << "The real signing journey requires GnuPG";
  const auto gpg = root / "gpg";
  fs::create_directories(gpg);
  fs::permissions(gpg, fs::perms::owner_all);
  writeFile(gpg / "gpg-agent.conf", "allow-loopback-pinentry\n");
  writeFile(gpg / "gpg.conf", "pinentry-mode loopback\n");
  struct AgentCleanup {
    fs::path home, root;
    ~AgentCleanup() {
      Child stop("/usr/bin/gpgconf",
                 {"--homedir", home.string(), "--kill", "gpg-agent"},
                 root / "stop", root / "stop.log");
      static_cast<void>(stop.wait(5s));
    }
  } cleanup{gpg, root};
  Child generate("/usr/bin/gpg",
                 {"--homedir", gpg.string(), "--batch", "--pinentry-mode",
                  "loopback", "--passphrase", "", "--quick-generate-key",
                  "Mesh fixture <mesh@test.invalid>", "ed25519", "sign", "0"},
                 root / "keygen", root / "keygen.log");
  auto generated = generate.wait(20s);
  ASSERT_TRUE(generated) << generate.output();
  ASSERT_EQ(*generated, 0) << generate.output();
  Child list("/usr/bin/gpg",
             {"--homedir", gpg.string(), "--batch", "--with-colons",
              "--list-secret-keys"},
             root / "keylist", root / "keylist.log");
  auto listed = list.wait(10s);
  ASSERT_TRUE(listed) << list.output();
  ASSERT_EQ(*listed, 0) << list.output();
  std::string signingKey;
  std::istringstream lines(list.output());
  for (std::string line; std::getline(lines, line);)
    if (line.starts_with("fpr:")) {
      std::istringstream fields(line);
      for (unsigned i = 0; i <= 9; ++i) std::getline(fields, signingKey, ':');
      break;
    }
  ASSERT_EQ(signingKey.size(), 40U) << list.output();
  auto requester = makeIdentity("requester"), worker = makeIdentity("worker");
  writeFile(root / "requester-master", requester.master);
  writeFile(root / "worker-master", worker.master);
  writeFile(root / "requester-delegation", requester.delegation.toTsv());
  writeFile(root / "worker-delegation", worker.delegation.toTsv());
  const auto requesterPort = freePort();
  auto workerPort          = freePort();
  while (workerPort == requesterPort) workerPort = freePort();
  const auto config = [&](std::string name, const ComputeIdentity &identity,
                          std::uint16_t local, std::uint16_t remote) {
    const auto profile = root / name;
    fs::create_directories(profile / "deployment");
    writeFile(profile / "device.secret", identity.keys.secretKey.hex());
    fs::permissions(profile / "device.secret",
                    fs::perms::owner_read | fs::perms::owner_write);
    const auto delegations = V::list(
        {V::dict({{"certificate",
                   V::string((root / "requester-delegation").string())},
                  {"master", V::string((root / "requester-master").string())}}),
         V::dict(
             {{"certificate", V::string((root / "worker-delegation").string())},
              {"master", V::string((root / "worker-master").string())}})});
    const auto configuration = V::dict(
        {{"version", V::integer(1)},
         {"author",
          V::string(identity.delegation.masterFingerprint.toString())},
         {"device", V::string(identity.keys.publicKey.hex())},
         {"secret", V::string("device.secret")},
         {"delegations", delegations},
         {"allow_compute",
          V::list(
              {V::string(requester.delegation.masterFingerprint.toString())})},
         {"deployment", V::string("deployment")},
         {"seeds", V::string("seeds")},
         {"results", V::string("results")},
         {"permascroll", V::string("permascroll")},
         {"downloads", V::string("downloads")},
         {"name", V::string("Mesh fixture")},
         {"email", V::string("mesh@test.invalid")},
         {"gpg_home", V::string(gpg.string())},
         {"gpg_key", V::string(signingKey)},
         {"budget", V::dict({{"cycles", V::integer(1000000)},
                             {"cells", V::integer(100000)},
                             {"solutions", V::integer(100)},
                             {"output", V::integer(1000000)}})},
         {"listen", V::string("127.0.0.1:" + std::to_string(local))},
         {"dht", V::integer(0)},
         {"peers", V::list({V::dict({{"address", V::string("127.0.0.1")},
                                     {"port", V::integer(remote)}})})},
         {"timeout", V::integer(25)}});
    const auto file = profile / "mesh.bencode";
    writeFile(file, configuration.encode());
    return file;
  };
  const auto requesterConfig =
      config("requester", requester, requesterPort, workerPort);
  const auto workerConfig = config("worker", worker, workerPort, requesterPort);
  Child server(binary("vpl"), {"--mesh-worker", workerConfig.string(), "180"},
               root / "worker", root / "worker.log");
  std::string workerNode;
  const auto started = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < started) {
    std::istringstream output(server.output());
    for (std::string line; std::getline(output, line);)
      if (line.starts_with("mesh-node ")) workerNode = line.substr(10);
    if (!workerNode.empty()) break;
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_FALSE(workerNode.empty()) << server.output();
  const std::pair<std::string_view, std::string_view> programs[] = {
      {"vpl", "1 + 2"},
      {"vquery", "##/d.remote%\"three\""},
      {"vprolog", "answer(3). ?- answer(X)."},
      {"vlace", "1 + 2"}};
  for (const auto &[frontend, source] : programs) {
    SCOPED_TRACE(frontend);
    const auto file = root / (std::string(frontend) + ".program");
    writeFile(file, source);
    Child client(
        binary(frontend),
        {"--mesh-run", requesterConfig.string(), workerNode, file.string()},
        root / "requester", root / (std::string(frontend) + "-run.log"));
    auto code = client.wait(45s);
    ASSERT_TRUE(code) << client.output() << "\nworker:\n" << server.output();
    ASSERT_EQ(*code, 0) << client.output() << "\nworker:\n" << server.output();
    EXPECT_NE(client.output().find("mesh-result "), std::string::npos);
    std::string saved;
    std::istringstream output(client.output());
    for (std::string line; std::getline(output, line);)
      if (line.starts_with("mesh-store ")) saved = line.substr(11);
    ASSERT_FALSE(saved.empty()) << client.output();
    EXPECT_TRUE(fs::exists(fs::path(saved) / "ops.nodes"));
    EXPECT_TRUE(fs::exists(fs::path(saved) / "store.tables"));
  }
}
#else
TEST(MeshCliTest, SubprocessJourneysRequirePosixProcessControl) {
  GTEST_SKIP() << "POSIX process control is required";
}
#endif
} // namespace
} // namespace zigzag::vortex::mesh

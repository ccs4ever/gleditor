#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "common/xanadu/vortex/mesh_swarm.hpp"
#include "identity_fixture.hpp"

namespace zigzag::vortex::mesh {
namespace {
using namespace std::chrono_literals;
namespace identity = xanadu::identity;

TEST(MeshSwarmTest, AuthenticatedControlSharesTheVerifiedContentListener) {
  xanadu::MutableKeys keys;
  keys.publicKey =
      xanadu::PublicKey::fromHex(xanadu::testing::kTestDevicePublicKeyHex);
  keys.secretKey =
      xanadu::SecretKey::fromHex(xanadu::testing::kTestDeviceSecretKeyHex);
  const auto fingerprint =
      *identity::Fingerprint::fromString(xanadu::testing::kAuthorFingerprint);
  const identity::IdentityNetworkController::Options identityOptions{
      .localFingerprint = fingerprint, .localDeviceKeys = keys};
  auto aIdentity =
      std::make_shared<identity::IdentityNetworkController>(identityOptions);
  auto bIdentity =
      std::make_shared<identity::IdentityNetworkController>(identityOptions);
  const auto delegation = xanadu::testing::fixtureDelegation(keys.publicKey);
  ASSERT_TRUE(aIdentity->trustDelegation(delegation,
                                         xanadu::testing::kAuthorPublicKey));
  ASSERT_TRUE(bIdentity->trustDelegation(delegation,
                                         xanadu::testing::kAuthorPublicKey));
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("xanadu-mesh-network-" + xanadu::createMutableKeys().publicKey.hex());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  auto options = [&](std::string suffix) {
    MeshTransportOptions options;
    options.downloads                            = directory / suffix;
    options.swarm.listenInterfaces               = "127.0.0.1:0";
    options.swarm.enableDht                      = false;
    options.swarm.enableLocalDiscovery           = false;
    options.swarm.enableTrackers                 = false;
    options.swarm.allowManyConnectionsPerAddress = true;
    options.swarm.readTimeout                    = 3s;
    options.swarm.metadataTimeout                = 3s;
    options.metadataTimeout                      = 3s;
    return options;
  };
  MeshSwarmTransport a(aIdentity, options("a")), b(bIdentity, options("b"));
  auto program = publishProgram("1 + 2", directory / "program");
  ASSERT_TRUE(program) << program.error();
  a.source().addTorrent(program->torrent.file, program->path.string(), true);
  b.source().addTorrent(program->torrent.file,
                        (directory / "download").string());
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  auto aPort          = a.source().listenPort();
  auto bPort          = b.source().listenPort();
  while ((!aPort || !bPort) && std::chrono::steady_clock::now() < deadline) {
    a.poll();
    b.poll();
    aPort = a.source().listenPort();
    bPort = b.source().listenPort();
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(aPort);
  ASSERT_TRUE(bPort);
  identity::ActorMessage message;
  message.type      = identity::MessageType::ActorSend;
  message.requestId = "same-socket-control";
  message.source    = identity::ActorPid{"node-a", "cursor-a", 1, 1};
  message.target    = identity::ActorPid{"node-b", "cursor-b", 1, 1};
  message.value     = "immutable control";
  ASSERT_TRUE(a.send(fingerprint, message));
  b.source().connectPeer(program->torrent.hash, "127.0.0.1", *aPort);
  std::vector<AuthenticatedActorEvent> incoming;
  while (incoming.empty() && std::chrono::steady_clock::now() < deadline) {
    a.poll();
    b.poll();
    incoming = b.takeActorMessages();
    if (incoming.empty()) std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(incoming.size(), 1U);
  EXPECT_EQ(incoming.front().identity, fingerprint);
  EXPECT_EQ(incoming.front().message, message);
  xanadu::Resolver reader(&b.source());
  const auto meta = b.source().metainfo(program->torrent.hash);
  ASSERT_TRUE(meta);
  const auto &file = meta->files().front();
  const auto value = reader.resolve(
      xanadu::Scroll::ofTorrentFile(program->torrent.hash, 0, file.path,
                                    file.offset, file.length),
      {.start = 0, .length = file.length});
  ASSERT_TRUE(value.isVerified());
  EXPECT_EQ(value.text, "1 + 2");
}
} // namespace
} // namespace zigzag::vortex::mesh

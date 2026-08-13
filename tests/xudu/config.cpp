/**
 * @file config.cpp
 * @brief Where somebody's name and signing key are kept, and how they are read.
 *
 * Identity belongs to a person rather than to a document: being asked to state
 * it once per store is how it ends up spelled three ways, or omitted, and an
 * omitted author is an unsigned document. So it lives in the per-user
 * configuration directory, and everything below is about that file being found
 * where XDG says, understood when it is read, and refused rather than guessed
 * at when it is something else.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <xudu/core/config.hpp>
#include <xudu/core/provenance.hpp>

namespace {

using xudu::Author;
using xudu::Config;

/// Environment variables put back the way they were found, since the test
/// binary runs every case in one process.
class Environment {
public:
  Environment() {
    remember("XUDU_CONFIG");
    remember("XDG_CONFIG_HOME");
    remember("HOME");
  }
  ~Environment() {
    for (const auto &[name, value] : saved) {
      if (value.empty()) {
        unsetenv(name.c_str());
      } else {
        setenv(name.c_str(), value.c_str(), 1);
      }
    }
  }

  Environment(const Environment &)            = delete;
  Environment &operator=(const Environment &) = delete;
  Environment(Environment &&)                 = delete;
  Environment &operator=(Environment &&)      = delete;

  static void set(const char *const name, const std::string &value) {
    setenv(name, value.c_str(), 1);
  }
  static void clear(const char *const name) { unsetenv(name); }

private:
  void remember(const char *const name) {
    const auto *const value = std::getenv(name);
    saved.emplace_back(name, nullptr == value ? std::string{} : value);
  }
  std::vector<std::pair<std::string, std::string>> saved;
};

std::string read(const std::filesystem::path &path) {
  std::ifstream in(path);
  return std::string{std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>()};
}

} // namespace

TEST(ConfigTest, everythingSetComesBackOut) {
  Config config;
  config.author.name   = "Ada Lovelace";
  config.author.email  = "ada@example.org";
  config.author.gpgKey = "0xDEADBEEF";
  config.gpgHome       = "/home/ada/.gnupg-publishing";

  const auto read = Config::fromYaml(config.toYaml());
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->author, config.author);
  EXPECT_EQ(read->gpgHome, config.gpgHome);
  EXPECT_TRUE(read->complete());
}

TEST(ConfigTest, aFileSaysWhatItIsForBeforeItSaysAnything) {
  // The audience is somebody opening it in an editor to change their name.
  const auto text = Config{}.toYaml();
  EXPECT_TRUE(text.starts_with("#"));
  EXPECT_TRUE(text.contains("publishes"));
}

TEST(ConfigTest, halfAnIdentityIsNotEnoughToSignWith) {
  Config config;
  EXPECT_FALSE(config.complete());
  config.author.name = "Ada Lovelace";
  EXPECT_FALSE(config.complete()) << "a name with no email names half a person";
  config.author.email = "ada@example.org";
  EXPECT_TRUE(config.complete());
}

// Settings a later version knows about are left alone rather than complained
// at: an old binary reading a new file should still find the author in it.
TEST(ConfigTest, unknownSettingsAreIgnoredAndTheRestIsRead) {
  const auto read = Config::fromYaml("author: \"Ada Lovelace\"\n"
                                     "email: \"ada@example.org\"\n"
                                     "future_setting: \"whatever\"\n");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->author.name, "Ada Lovelace");
  EXPECT_EQ(read->author.email, "ada@example.org");
}

TEST(ConfigTest, somethingThatIsNotAConfigurationIsRefused) {
  // Not read as an empty configuration: the file was written by somebody who
  // meant something by it, and quietly publishing as nobody is the failure
  // this is trying to avoid.
  EXPECT_FALSE(Config::fromYaml("this is not a configuration").has_value());
}

TEST(ConfigTest, theFileIsWhereXdgSaysItIs) {
  const Environment environment;
  Environment::clear("XUDU_CONFIG");

  Environment::set("XDG_CONFIG_HOME", "/somewhere/config");
  EXPECT_EQ(xudu::configPath(), "/somewhere/config/xudu/config.yaml");

  // Without it, the fallback the specification names.
  Environment::clear("XDG_CONFIG_HOME");
  Environment::set("HOME", "/home/ada");
  EXPECT_EQ(xudu::configPath(), "/home/ada/.config/xudu/config.yaml");

  // And a file named outright wins over both, which is what lets somebody with
  // two identities keep two.
  Environment::set("XUDU_CONFIG", "/tmp/other.yaml");
  EXPECT_EQ(xudu::configPath(), "/tmp/other.yaml");
}

TEST(ConfigTest, itIsWrittenReadableOnlyByItsOwner) {
  const Environment environment;
  const auto path = std::filesystem::temp_directory_path() /
                    ("xudu-config-" + std::to_string(getpid())) /
                    "config.yaml";
  std::filesystem::remove_all(path.parent_path());

  Config config;
  config.author.name  = "Ada Lovelace";
  config.author.email = "ada@example.org";
  config.gpgHome      = "/media/key/.gnupg";
  xudu::saveConfig(config, path.string());

  // It says who somebody is and where their signing key lives.
  const auto mode = std::filesystem::status(path).permissions();
  EXPECT_EQ(mode & std::filesystem::perms::group_all,
            std::filesystem::perms::none);
  EXPECT_EQ(mode & std::filesystem::perms::others_all,
            std::filesystem::perms::none);

  const auto back = xudu::loadConfig(path.string());
  EXPECT_EQ(back.author, config.author);
  EXPECT_EQ(back.gpgHome, config.gpgHome);
  EXPECT_TRUE(read(path).contains("Ada Lovelace"));

  std::filesystem::remove_all(path.parent_path());
}

TEST(ConfigTest, noFileIsAnEmptyConfigurationRatherThanAnError) {
  const auto missing = std::filesystem::temp_directory_path() /
                       "xudu-config-that-is-not-there.yaml";
  std::filesystem::remove(missing);
  const auto config = xudu::loadConfig(missing.string());
  EXPECT_FALSE(config.complete());
  EXPECT_TRUE(config.author.name.empty());
}

TEST(ConfigTest, aFileThatCannotBeUnderstoodIsAnError) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("xudu-bad-config-" + std::to_string(getpid()) + ".yaml");
  {
    std::ofstream out(path);
    out << "not a configuration at all\n";
  }
  EXPECT_THROW(static_cast<void>(xudu::loadConfig(path.string())),
               std::runtime_error);
  std::filesystem::remove(path);
}

// Which key to sign with is chosen from what the keyring holds, so what the
// keyring holds has to be askable -- and one of them has to be the one gpg
// would reach for on its own, since that is what somebody who has said nothing
// expects to be used.
TEST(ConfigTest, theKeyringSaysWhatItCanSignWith) {
  const Environment environment;
  const auto dir = std::filesystem::temp_directory_path() /
                   ("xudu-keyring-" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  {
    std::ofstream(dir / "gpg-agent.conf") << "allow-loopback-pinentry\n";
    std::ofstream(dir / "gpg.conf") << "pinentry-mode loopback\n";
  }

  xudu::SigningOptions where;
  where.gpgHome = dir.string();
  EXPECT_TRUE(xudu::signingKeys(where).empty())
      << "an empty keyring has nothing to offer";

  const auto make = [&dir](const std::string &who) {
    return 0 == std::system(("gpg --homedir " + dir.string() +
                             " --batch --passphrase '' --quick-generate-key '" +
                             who + "' ed25519 sign never >/dev/null 2>&1")
                                .c_str());
  };
  if (!make("Ada Lovelace <ada@example.org>") ||
      !make("Grace Hopper <grace@example.org>")) {
    static_cast<void>(std::system(("gpgconf --homedir " + dir.string() +
                                   " --kill gpg-agent >/dev/null 2>&1")
                                      .c_str()));
    std::filesystem::remove_all(dir);
    GTEST_SKIP() << "no gpg keyring could be made here";
  }

  const auto keys = xudu::signingKeys(where);
  ASSERT_EQ(keys.size(), 2U);
  for (const auto &key : keys) {
    // A fingerprint, not a short id: a short id is a prefix, and prefixes
    // collide.
    EXPECT_EQ(key.fingerprint.size(), 40U);
    EXPECT_TRUE(key.identity.contains("@example.org")) << key.identity;
    // What somebody picking between them reads: who it is, and enough of the
    // fingerprint to tell two of the same name apart.
    EXPECT_TRUE(key.describe().contains(key.identity));
    EXPECT_TRUE(key.describe().contains(
        key.fingerprint.substr(key.fingerprint.size() - 8)));
  }
  EXPECT_EQ(std::ranges::count_if(keys,
                                  [](const xudu::SigningKey &key) {
                                    return key.preferred;
                                  }),
            1)
      << "exactly one key is the one gpg would use";

  static_cast<void>(std::system(("gpgconf --homedir " + dir.string() +
                                 " --kill gpg-agent >/dev/null 2>&1")
                                    .c_str()));
  std::filesystem::remove_all(dir);
}

// A passphrase goes to gpg down a pipe rather than on a command line, which is
// readable by every process on the machine. What is checked here is that it
// works at all: a key with a passphrase cannot be signed with without one.
TEST(ConfigTest, aKeyWithAPassphraseIsSignedWithWhenGivenOne) {
  const Environment environment;
  const auto dir = std::filesystem::temp_directory_path() /
                   ("xudu-passphrase-" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  {
    std::ofstream(dir / "gpg-agent.conf")
        << "allow-loopback-pinentry\nmax-cache-ttl 0\ndefault-cache-ttl 0\n";
  }
  const bool made =
      0 == std::system(("gpg --homedir " + dir.string() +
                        " --batch --pinentry-mode loopback --passphrase "
                        "'correct horse' --quick-generate-key "
                        "'Locked Key <locked@example.org>' ed25519 sign never "
                        ">/dev/null 2>&1")
                           .c_str());
  if (!made) {
    std::filesystem::remove_all(dir);
    GTEST_SKIP() << "no gpg keyring could be made here";
  }

  Config config;
  config.author.name   = "Locked Key";
  config.author.email  = "locked@example.org";
  config.author.gpgKey = "locked@example.org";
  config.gpgHome       = dir.string();

  xudu::Provenance record;
  record.author = config.author;
  record.title  = "Signed with a passphrase";

  const auto signed_ =
      xudu::signProvenance(record, config.signing("correct horse"));
  EXPECT_TRUE(signed_.signature.starts_with("-----BEGIN PGP SIGNATURE-----"));
  EXPECT_TRUE(xudu::verifyProvenance(signed_, config.signing()).signatureValid);

  static_cast<void>(std::system(("gpgconf --homedir " + dir.string() +
                                 " --kill gpg-agent >/dev/null 2>&1")
                                    .c_str()));
  std::filesystem::remove_all(dir);
}

// vi: set sw=2 sts=2 ts=2 et:

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
  config.gpgSecretKey  = "/home/ada/keys/ada.sec.asc";

  const auto read = Config::fromYaml(config.toYaml());
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->author, config.author);
  EXPECT_EQ(read->gpgHome, config.gpgHome);
  EXPECT_EQ(read->gpgSecretKey, config.gpgSecretKey);
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
  config.gpgSecretKey = "/home/ada/keys/ada.sec.asc";
  xudu::saveConfig(config, path.string());

  // It says who somebody is and where their signing key lives.
  const auto mode = std::filesystem::status(path).permissions();
  EXPECT_EQ(mode & std::filesystem::perms::group_all,
            std::filesystem::perms::none);
  EXPECT_EQ(mode & std::filesystem::perms::others_all,
            std::filesystem::perms::none);

  const auto back = xudu::loadConfig(path.string());
  EXPECT_EQ(back.author, config.author);
  EXPECT_EQ(back.gpgSecretKey, config.gpgSecretKey);
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

// The spelling somebody reaches for first, and the one gpg stopped supporting
// directly: a key named as a file is imported into a keyring of its own for
// the one signature.
TEST(ConfigTest, aSecretKeyGivenAsAFileCanSign) {
  const Environment environment;
  const auto dir = std::filesystem::temp_directory_path() /
                   ("xudu-keyfile-" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);

  // A keyring to make the key in, thrown away afterwards. Kept apart from
  // whatever keyring is on the machine running this.
  Environment::set("GNUPGHOME", dir.string());
  {
    std::ofstream(dir / "gpg-agent.conf") << "allow-loopback-pinentry\n";
    std::ofstream(dir / "gpg.conf") << "pinentry-mode loopback\n";
  }
  const bool made =
      0 == std::system("gpg --batch --passphrase '' --quick-generate-key "
                       "'Grace Hopper <grace@example.org>' ed25519 sign never "
                       ">/dev/null 2>&1");
  const auto keyFile = dir / "grace.sec.asc";
  const bool exported =
      made && 0 == std::system(("gpg --batch --pinentry-mode loopback "
                                "--passphrase '' --armor --export-secret-keys "
                                "grace@example.org > " +
                                keyFile.string() + " 2>/dev/null")
                                   .c_str());
  if (!exported || std::filesystem::file_size(keyFile) == 0) {
    static_cast<void>(std::system("gpgconf --kill gpg-agent >/dev/null 2>&1"));
    std::filesystem::remove_all(dir);
    Environment::clear("GNUPGHOME");
    GTEST_SKIP() << "no gpg keyring could be made here";
  }

  // Now with no keyring at all: the key file is the only way in.
  const auto elsewhere = dir / "empty-home";
  std::filesystem::create_directories(elsewhere);
  Environment::set("GNUPGHOME", elsewhere.string());

  Config config;
  config.author.name   = "Grace Hopper";
  config.author.email  = "grace@example.org";
  config.gpgSecretKey  = keyFile.string();

  xudu::Provenance record;
  record.author = config.author;
  record.title  = "Signed from a key file";

  const auto signed_ = xudu::signProvenance(record, config.signing());
  EXPECT_TRUE(signed_.signature.starts_with("-----BEGIN PGP SIGNATURE-----"));

  static_cast<void>(std::system("gpgconf --kill gpg-agent >/dev/null 2>&1"));
  std::filesystem::remove_all(dir);
  Environment::clear("GNUPGHOME");
}

// vi: set sw=2 sts=2 ts=2 et:

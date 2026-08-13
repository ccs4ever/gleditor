/**
 * @file provenance.cpp
 * @brief Implementation of the signed authorship record.
 */
#include "provenance.hpp" // IWYU pragma: associated

#include "yaml.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glibmm/checksum.h>

extern char **environ;

namespace xudu {

namespace {

/// What running a program produced.
struct Ran {
  bool started{}; ///< False when the program could not be run at all.
  int status{};   ///< Its exit status, when it ran.
  std::string out;
  std::string err;

  [[nodiscard]] bool ok() const { return started && 0 == status; }
  /// Whatever it complained with, for putting in an exception.
  [[nodiscard]] std::string complaint() const {
    auto said = err.empty() ? out : err;
    while (!said.empty() && ('\n' == said.back() || '\r' == said.back())) {
      said.pop_back();
    }
    return said;
  }
};

/**
 * @brief Run a program and collect what it said.
 *
 * An argument vector rather than a command line: an author's name is somebody
 * else's text, and handing text to a shell is how text becomes commands.
 * Nothing here goes near /bin/sh.
 */
Ran run(const std::vector<std::string> &argv) {
  std::array<int, 2> outPipe{};
  std::array<int, 2> errPipe{};
  if (0 != pipe(outPipe.data()) || 0 != pipe(errPipe.data())) {
    return {};
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, outPipe[0]);
  posix_spawn_file_actions_addclose(&actions, errPipe[0]);
  posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, outPipe[1]);
  posix_spawn_file_actions_addclose(&actions, errPipe[1]);

  std::vector<char *> raw;
  raw.reserve(argv.size() + 1);
  for (const auto &arg : argv) {
    raw.push_back(const_cast<char *>(arg.c_str()));
  }
  raw.push_back(nullptr);

  pid_t child = 0;
  const auto spawned =
      posix_spawnp(&child, raw[0], &actions, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(outPipe[1]);
  close(errPipe[1]);
  if (0 != spawned) {
    close(outPipe[0]);
    close(errPipe[0]);
    return {};
  }

  // Both pipes are drained before waiting, or a program that fills one while
  // this waits on the other deadlocks.
  const auto drain = [](const int fd, std::string &into) {
    std::array<char, 4096> buffer{};
    while (true) {
      const auto got = read(fd, buffer.data(), buffer.size());
      if (got <= 0) {
        break;
      }
      into.append(buffer.data(), static_cast<std::size_t>(got));
    }
    close(fd);
  };
  Ran ran;
  ran.started = true;
  drain(outPipe[0], ran.out);
  drain(errPipe[0], ran.err);

  int status = 0;
  while (-1 == waitpid(child, &status, 0) && EINTR == errno) {
  }
  ran.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return ran;
}

/// A file that deletes itself, since gpg wants paths and this wants none of
/// them left behind.
class Scratch {
public:
  Scratch(const std::string &suffix, const std::string_view contents) {
    static int counter = 0;
    path = std::filesystem::temp_directory_path() /
           std::format("xudu-{}-{}{}", getpid(), counter++, suffix);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  ~Scratch() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  Scratch(const Scratch &)            = delete;
  Scratch &operator=(const Scratch &) = delete;
  Scratch(Scratch &&)                 = delete;
  Scratch &operator=(Scratch &&)      = delete;

  [[nodiscard]] std::string name() const { return path.string(); }
  [[nodiscard]] std::string read() const {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
  }

private:
  std::filesystem::path path;
};

} // namespace

std::string Provenance::toYaml() const {
  // A comment first, because the audience for this file is a person deciding
  // whether to believe it, and they may never have seen one before.
  std::string out =
      "# Authorship of a xanadoc, signed with OpenPGP before the content was\n"
      "# sealed into a torrent, and sealed into it alongside the content. The\n"
      "# signature is in " +
      std::string{provenanceSigName} +
      "; check it with:\n"
      "#   gpg --verify " +
      std::string{provenanceSigName} + " " + std::string{provenanceFileName} +
      "\n";

  yaml::write(out, "author", author.name);
  yaml::write(out, "email", author.email);
  yaml::write(out, "gpg_key", author.gpgKey);
  yaml::write(out, "title", title);
  yaml::write(out, "salt", salt);
  yaml::write(out, "publisher", publisher);
  yaml::write(out, "version", version);
  if (0 != published) {
    out += std::format("published: {}\n", published);
  }
  out += std::format("content_length: {}\n", contentLength);
  yaml::write(out, "content_sha256", contentDigest);
  for (const auto &[key, value] : extra) {
    yaml::write(out, key, value);
  }
  yaml::writeList(out, "quotes", quotes);
  return out;
}

std::string sha256Hex(const std::string_view data) {
  Glib::Checksum checksum(Glib::Checksum::Type::SHA256);
  checksum.update(reinterpret_cast<const guchar *>(data.data()), data.size());
  return checksum.get_string();
}

namespace {

/// A GnuPG home of its own, thrown away afterwards. Short, because the agent's
/// socket lives in it and a long path is longer than a unix socket name may be
/// -- which gpg reports as "no agent running", and is nothing of the kind.
class Keyring {
public:
  explicit Keyring(const std::string &secretKeyFile) {
    path = std::filesystem::temp_directory_path() /
           std::format("xudu-key-{}", getpid());
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 ignored);
    // No terminal to ask for a passphrase on, so a protected key will be
    // refused here rather than hanging. gpg says which, and that is the
    // actionable part.
    { std::ofstream(path / "gpg-agent.conf") << "allow-loopback-pinentry\n"; }
    { std::ofstream(path / "gpg.conf") << "pinentry-mode loopback\n"; }

    imported = run({"gpg", "--homedir", path.string(), "--batch", "--yes",
                    "--import", secretKeyFile});
  }
  ~Keyring() {
    static_cast<void>(run({"gpgconf", "--homedir", path.string(), "--kill",
                           "gpg-agent"}));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  Keyring(const Keyring &)            = delete;
  Keyring &operator=(const Keyring &) = delete;
  Keyring(Keyring &&)                 = delete;
  Keyring &operator=(Keyring &&)      = delete;

  [[nodiscard]] bool ok() const { return imported.ok(); }
  [[nodiscard]] std::string complaint() const { return imported.complaint(); }
  [[nodiscard]] std::string home() const { return path.string(); }

private:
  std::filesystem::path path;
  Ran imported;
};

/// The `--homedir` gpg should be run with, if any.
void addHome(std::vector<std::string> &argv, const std::string &home) {
  if (home.empty()) {
    return;
  }
  argv.emplace_back("--homedir");
  argv.push_back(home);
}

} // namespace

bool gpgAvailable() { return run({"gpg", "--version"}).ok(); }

SignedProvenance signProvenance(const Provenance &record,
                                const SigningOptions &where) {
  if (!record.author.named()) {
    throw std::runtime_error(
        "cannot sign for an author with no name and email. Publishing says who "
        "wrote something, and an unsigned claim of authorship is worth what "
        "anybody's claim about anybody is worth -- give --author-name and "
        "--author-email once and they are kept with the store.");
  }
  if (!gpgAvailable()) {
    throw std::runtime_error(
        "gpg was not found. Provenance is an OpenPGP signature over the "
        "authorship record, made before the content is sealed, so publishing "
        "needs GnuPG installed.");
  }

  SignedProvenance out;
  out.yaml = record.toYaml();

  const Scratch document(".yaml", out.yaml);
  const Scratch signature(".yaml.asc", "");

  // A key given as a file is imported into a keyring of its own for this one
  // signature; a GnuPG home is used as it is. Named separately because they
  // are different things: one is a key, the other is where keys are kept.
  std::optional<Keyring> imported;
  if (!where.secretKeyFile.empty()) {
    imported.emplace(where.secretKeyFile);
    if (!imported->ok()) {
      throw std::runtime_error(std::format(
          "gpg would not read the signing key at {}: {}", where.secretKeyFile,
          imported->complaint()));
    }
  }
  const auto home = imported ? imported->home() : where.gpgHome;

  std::vector<std::string> argv{"gpg", "--batch", "--yes", "--armor"};
  addHome(argv, home);
  if (!record.author.gpgKey.empty()) {
    argv.emplace_back("--local-user");
    argv.push_back(record.author.gpgKey);
  }
  argv.emplace_back("--output");
  argv.push_back(signature.name());
  argv.emplace_back("--detach-sign");
  argv.push_back(document.name());

  const auto ran = run(argv);
  if (!ran.ok()) {
    throw std::runtime_error(std::format(
        "gpg would not sign the authorship record{}: {}",
        record.author.gpgKey.empty() ? std::string{}
                                     : " as " + record.author.gpgKey,
        ran.complaint()));
  }
  out.signature = signature.read();
  if (out.signature.empty()) {
    throw std::runtime_error("gpg reported success and produced no signature");
  }
  return out;
}

ProvenanceCheck verifyProvenance(const SignedProvenance &signed_,
                                 const SigningOptions &where) {
  ProvenanceCheck check;
  if (signed_.signature.empty()) {
    check.detail = "there is no signature";
    return check;
  }
  if (!gpgAvailable()) {
    check.detail = "gpg is not installed, so the signature cannot be checked";
    return check;
  }

  const Scratch document(".yaml", signed_.yaml);
  const Scratch signature(".yaml.asc", signed_.signature);

  // --status-fd is the machine-readable channel; the human text on stderr says
  // different things in different locales and versions, and is kept only to be
  // shown when something went wrong.
  std::vector<std::string> argv{"gpg", "--batch"};
  addHome(argv, where.gpgHome);
  argv.insert(argv.end(), {"--status-fd", "1", "--verify", signature.name(),
                           document.name()});
  const auto ran = run(argv);
  check.detail   = ran.complaint();
  if (!ran.started) {
    return check;
  }

  std::istringstream lines(ran.out);
  std::string line;
  while (std::getline(lines, line)) {
    constexpr std::string_view prefix = "[GNUPG:] ";
    if (!line.starts_with(prefix)) {
      continue;
    }
    std::istringstream fields(line.substr(prefix.size()));
    std::string keyword;
    fields >> keyword;
    if ("GOODSIG" == keyword) {
      check.signatureValid = true;
      std::string keyId;
      fields >> keyId;
      std::getline(fields, check.signer);
      // GOODSIG's identity is the rest of the line, which begins with the
      // space that separated it from the key id.
      if (!check.signer.empty() && ' ' == check.signer.front()) {
        check.signer.erase(0, 1);
      }
    } else if ("VALIDSIG" == keyword) {
      // The last field of VALIDSIG is the primary key's fingerprint; the first
      // is the fingerprint of the key that actually signed, which for a signing
      // subkey is not the same thing.
      std::vector<std::string> parts;
      std::string part;
      while (fields >> part) {
        parts.push_back(part);
      }
      if (!parts.empty()) {
        check.fingerprint = parts.back();
      }
    } else if ("TRUST_ULTIMATE" == keyword || "TRUST_FULLY" == keyword ||
               "TRUST_MARGINAL" == keyword) {
      check.keyTrusted = true;
    }
  }
  return check;
}

std::optional<Provenance> parseProvenance(const std::string_view text) {
  const auto entries = yaml::read(text);
  if (!entries) {
    return std::nullopt;
  }

  Provenance out;
  bool sawAuthor = false;
  for (const auto &[key, value, listItem] : *entries) {
    if (listItem) {
      if ("quotes" == key) {
        out.quotes.push_back(value);
      }
      continue;
    }
    if ("author" == key) {
      out.author.name = value;
      sawAuthor       = true;
    } else if ("email" == key) {
      out.author.email = value;
    } else if ("gpg_key" == key) {
      out.author.gpgKey = value;
    } else if ("title" == key) {
      out.title = value;
    } else if ("salt" == key) {
      out.salt = value;
    } else if ("publisher" == key) {
      out.publisher = value;
    } else if ("version" == key) {
      out.version = value;
    } else if ("published" == key) {
      out.published = std::strtoull(value.c_str(), nullptr, 10);
    } else if ("content_length" == key) {
      out.contentLength = std::strtoull(value.c_str(), nullptr, 10);
    } else if ("content_sha256" == key) {
      out.contentDigest = value;
    } else if ("quotes" != key) {
      // Anything this version does not know about is kept rather than
      // dropped: a record written by a later one still says what it said, and
      // a reader that quietly discarded half of it would be showing somebody
      // a claim that is not the claim that was signed.
      out.extra.emplace_back(key, value);
    }
  }
  if (!sawAuthor) {
    return std::nullopt;
  }
  return out;
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:

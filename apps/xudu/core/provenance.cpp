/**
 * @file provenance.cpp
 * @brief Implementation of the signed authorship record.
 */
#include "provenance.hpp" // IWYU pragma: associated

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
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

/// A YAML double-quoted scalar. Everything is quoted rather than only what has
/// to be: a name with a colon in it is ordinary, and a rule with no exceptions
/// is one nobody has to remember.
std::string asYamlString(const std::string_view text) {
  std::string out = "\"";
  for (const char chr : text) {
    switch (chr) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += chr;
    }
  }
  out += '"';
  return out;
}

/// The inverse, for the small reader below.
std::string fromYamlString(std::string_view text) {
  if (text.size() < 2 || '"' != text.front() || '"' != text.back()) {
    return std::string{text};
  }
  text.remove_prefix(1);
  text.remove_suffix(1);
  std::string out;
  for (std::size_t i = 0; i < text.size(); i++) {
    if ('\\' != text[i] || i + 1 == text.size()) {
      out += text[i];
      continue;
    }
    switch (text[++i]) {
    case 'n':
      out += '\n';
      break;
    case 'r':
      out += '\r';
      break;
    case 't':
      out += '\t';
      break;
    default:
      out += text[i];
    }
  }
  return out;
}

void writeField(std::string &out, const std::string_view key,
                const std::string_view value) {
  if (value.empty()) {
    return;
  }
  out += std::format("{}: {}\n", key, asYamlString(value));
}

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && (' ' == text.front() || '\t' == text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (' ' == text.back() || '\t' == text.back() ||
                           '\r' == text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

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

  writeField(out, "author", author.name);
  writeField(out, "email", author.email);
  writeField(out, "gpg_key", author.gpgKey);
  writeField(out, "title", title);
  writeField(out, "salt", salt);
  writeField(out, "publisher", publisher);
  writeField(out, "version", version);
  if (0 != published) {
    out += std::format("published: {}\n", published);
  }
  out += std::format("content_length: {}\n", contentLength);
  writeField(out, "content_sha256", contentDigest);
  for (const auto &[key, value] : extra) {
    writeField(out, key, value);
  }
  if (!quotes.empty()) {
    out += "quotes:\n";
    for (const auto &key : quotes) {
      out += std::format("  - {}\n", asYamlString(key));
    }
  }
  return out;
}

std::string sha256Hex(const std::string_view data) {
  Glib::Checksum checksum(Glib::Checksum::Type::SHA256);
  checksum.update(reinterpret_cast<const guchar *>(data.data()), data.size());
  return checksum.get_string();
}

bool gpgAvailable() { return run({"gpg", "--version"}).ok(); }

SignedProvenance signProvenance(const Provenance &record) {
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

  std::vector<std::string> argv{"gpg", "--batch", "--yes", "--armor"};
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

ProvenanceCheck verifyProvenance(const SignedProvenance &signed_) {
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
  const auto ran = run({"gpg", "--batch", "--status-fd", "1", "--verify",
                        signature.name(), document.name()});
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
      check.signer = std::string{trimmed(check.signer)};
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

std::optional<Provenance> parseProvenance(const std::string_view yaml) {
  Provenance out;
  bool sawAuthor = false;
  bool inQuotes  = false;

  std::istringstream lines{std::string{yaml}};
  std::string raw;
  while (std::getline(lines, raw)) {
    const auto line = trimmed(raw);
    if (line.empty() || line.starts_with("#")) {
      continue;
    }
    if (line.starts_with("- ")) {
      if (!inQuotes) {
        return std::nullopt;
      }
      out.quotes.push_back(fromYamlString(trimmed(line.substr(2))));
      continue;
    }
    const auto colon = line.find(':');
    if (std::string_view::npos == colon) {
      return std::nullopt;
    }
    const auto key   = std::string{trimmed(line.substr(0, colon))};
    const auto value = fromYamlString(trimmed(line.substr(colon + 1)));
    inQuotes         = "quotes" == key;

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

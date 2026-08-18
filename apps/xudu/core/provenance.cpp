/**
 * @file provenance.cpp
 * @brief Implementation of the signed authorship record.
 */
#include "provenance.hpp" // IWYU pragma: associated

#include "windows_quoting.hpp"
#include "yaml.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
// NOMINMAX: windows.h's own min/max macros would otherwise shadow
// std::max below, and silently pick the wrong one. MinGW predefines this
// already; MSVC, if this is ever built with it, does not.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on
#include <process.h> // _getpid
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

#include <openssl/sha.h>

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

#if defined(_WIN32)

/**
 * @brief An argv element, converted to UTF-16 and quoted as one token of a
 *        Windows command line.
 *
 * CreateProcess takes a single string rather than an array, so an argument
 * vector has to be joined into one -- and that join is the whole of what
 * would otherwise be a shell's job, done without a shell. The conversion is
 * the only part of this that touches the Windows API; the quoting itself is
 * pure string handling and lives in windows_quoting.cpp, where it can be
 * checked without a Windows machine to run it on.
 */
std::wstring quoteForWindows(const std::string &argUtf8) {
  const auto wideLength = MultiByteToWideChar(
      CP_UTF8, 0, argUtf8.data(), static_cast<int>(argUtf8.size()), nullptr, 0);
  std::wstring arg(static_cast<std::size_t>(std::max(wideLength, 0)), L'\0');
  if (wideLength > 0) {
    MultiByteToWideChar(CP_UTF8, 0, argUtf8.data(),
                        static_cast<int>(argUtf8.size()), arg.data(),
                        wideLength);
  }
  return quoteWindowsArgument(arg);
}

/// A pipe with both ends closed on destruction unless taken. RAII over a pair
/// of handles is what keeps every early return above from leaking one.
struct Pipe {
  HANDLE readEnd{INVALID_HANDLE_VALUE};
  HANDLE writeEnd{INVALID_HANDLE_VALUE};

  Pipe() {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    static_cast<void>(CreatePipe(&readEnd, &writeEnd, &inheritable, 0));
  }
  ~Pipe() { close(); }
  Pipe(const Pipe &)            = delete;
  Pipe &operator=(const Pipe &) = delete;
  Pipe(Pipe &&)                 = delete;
  Pipe &operator=(Pipe &&)      = delete;

  [[nodiscard]] bool ok() const {
    return INVALID_HANDLE_VALUE != readEnd && INVALID_HANDLE_VALUE != writeEnd;
  }
  void closeRead() {
    if (INVALID_HANDLE_VALUE != readEnd) {
      CloseHandle(readEnd);
      readEnd = INVALID_HANDLE_VALUE;
    }
  }
  void closeWrite() {
    if (INVALID_HANDLE_VALUE != writeEnd) {
      CloseHandle(writeEnd);
      writeEnd = INVALID_HANDLE_VALUE;
    }
  }
  void close() {
    closeRead();
    closeWrite();
  }
};

/**
 * @brief Run a program and collect what it said.
 *
 * An argument vector rather than a command line: an author's name is somebody
 * else's text, and handing text to a shell is how text becomes commands.
 * Nothing here goes near cmd.exe -- CreateProcess is given the one string a
 * command line has to be, but it is built by quoteForWindows() rather than by
 * concatenation, which is what keeps an argument from being read as two.
 */
Ran run(const std::vector<std::string> &argv,
        const std::string_view feed = {}) {
  Pipe outPipe;
  Pipe errPipe;
  Pipe inPipe;
  if (!outPipe.ok() || !errPipe.ok() || !inPipe.ok()) {
    return {};
  }
  // The end each side keeps must not be inherited, or a pipe meant to signal
  // end-of-file by closing never does: the child's copy of the parent's own
  // end keeps it open.
  static_cast<void>(
      SetHandleInformation(outPipe.readEnd, HANDLE_FLAG_INHERIT, 0));
  static_cast<void>(
      SetHandleInformation(errPipe.readEnd, HANDLE_FLAG_INHERIT, 0));
  static_cast<void>(
      SetHandleInformation(inPipe.writeEnd, HANDLE_FLAG_INHERIT, 0));

  std::wstring commandLine;
  for (const auto &arg : argv) {
    if (!commandLine.empty()) {
      commandLine.push_back(L' ');
    }
    commandLine += quoteForWindows(arg);
  }

  STARTUPINFOW startup{};
  startup.cb         = sizeof(startup);
  startup.dwFlags    = STARTF_USESTDHANDLES;
  startup.hStdInput  = inPipe.readEnd;
  startup.hStdOutput = outPipe.writeEnd;
  startup.hStdError  = errPipe.writeEnd;

  PROCESS_INFORMATION process{};
  // CreateProcess may write into the buffer it is given, so the command line
  // cannot be a literal or a const pointer into commandLine.
  const auto spawned =
      CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0,
                     nullptr, nullptr, &startup, &process);
  outPipe.closeWrite();
  errPipe.closeWrite();
  inPipe.closeRead();
  if (0 == spawned) {
    return {};
  }
  CloseHandle(process.hThread);

  // What goes down the pipe is a passphrase and nothing longer, so one write
  // cannot block: it is far below what a pipe holds without a reader.
  if (!feed.empty()) {
    DWORD written = 0;
    static_cast<void>(WriteFile(inPipe.writeEnd, feed.data(),
                                static_cast<DWORD>(feed.size()), &written,
                                nullptr));
  }
  // Closed either way, so a program waiting on end of input is not left
  // waiting for a passphrase that is not coming.
  inPipe.closeWrite();

  // Both pipes are drained before waiting, or a program that fills one while
  // this waits on the other deadlocks.
  const auto drain = [](HANDLE &handle, std::string &into) {
    std::array<char, 4096> buffer{};
    while (true) {
      DWORD got = 0;
      if (0 == ReadFile(handle, buffer.data(),
                        static_cast<DWORD>(buffer.size()), &got, nullptr) ||
          0 == got) {
        break;
      }
      into.append(buffer.data(), static_cast<std::size_t>(got));
    }
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
  };
  Ran ran;
  ran.started = true;
  drain(outPipe.readEnd, ran.out);
  drain(errPipe.readEnd, ran.err);

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hProcess);
  ran.status = static_cast<int>(exitCode);
  return ran;
}

#else

/**
 * @brief Run a program and collect what it said.
 *
 * An argument vector rather than a command line: an author's name is somebody
 * else's text, and handing text to a shell is how text becomes commands.
 * Nothing here goes near /bin/sh.
 */
Ran run(const std::vector<std::string> &argv,
        const std::string_view feed = {}) {
  std::array<int, 2> outPipe{};
  std::array<int, 2> errPipe{};
  std::array<int, 2> inPipe{};
  if (0 != pipe(outPipe.data()) || 0 != pipe(errPipe.data()) ||
      0 != pipe(inPipe.data())) {
    return {};
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, outPipe[0]);
  posix_spawn_file_actions_addclose(&actions, errPipe[0]);
  posix_spawn_file_actions_addclose(&actions, inPipe[1]);
  posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
  posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
  posix_spawn_file_actions_addclose(&actions, outPipe[1]);
  posix_spawn_file_actions_addclose(&actions, errPipe[1]);
  posix_spawn_file_actions_addclose(&actions, inPipe[0]);

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
  close(inPipe[0]);
  if (0 != spawned) {
    close(outPipe[0]);
    close(errPipe[0]);
    close(inPipe[1]);
    return {};
  }

  // What goes down the pipe is a passphrase and nothing longer, so one write
  // cannot block: it is far below what a pipe holds without a reader.
  if (!feed.empty()) {
    static_cast<void>(write(inPipe[1], feed.data(), feed.size()));
  }
  // Closed either way, so a program waiting on end of input is not left
  // waiting for a passphrase that is not coming.
  close(inPipe[1]);

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

#endif // defined(_WIN32)

/// A file that deletes itself, since gpg wants paths and this wants none of
/// them left behind.
class Scratch {
public:
  Scratch(const std::string &suffix, const std::string_view contents) {
    static int counter = 0;
    path               = std::filesystem::temp_directory_path() /
#if defined(_WIN32)
           std::format("xudu-{}-{}{}", _getpid(), counter++, suffix);
#else
           std::format("xudu-{}-{}{}", getpid(), counter++, suffix);
#endif
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
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(),
         digest.data());
  static constexpr char hexDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    hex.push_back(hexDigits[(byte >> 4) & 0x0F]);
    hex.push_back(hexDigits[byte & 0x0F]);
  }
  return hex;
}

namespace {

/// The `--homedir` gpg should be run with, if any.
void addHome(std::vector<std::string> &argv, const std::string &home) {
  if (home.empty()) {
    return;
  }
  argv.emplace_back("--homedir");
  argv.push_back(home);
}

/// The colon-separated fields of one line of gpg's machine-readable output.
std::vector<std::string> colonFields(const std::string_view line) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (true) {
    const auto colon = line.find(':', at);
    if (std::string_view::npos == colon) {
      out.emplace_back(line.substr(at));
      return out;
    }
    out.emplace_back(line.substr(at, colon - at));
    at = colon + 1;
  }
}

/// What `default-key` is set to, or nothing. gpgconf rather than reading
/// gpg.conf: the option may come from any of the places gpg reads, and gpgconf
/// is what knows which of them won.
std::string configuredDefaultKey(const std::string &home) {
  std::vector<std::string> argv{"gpgconf"};
  addHome(argv, home);
  argv.insert(argv.end(), {"--list-options", "gpg"});
  const auto ran = run(argv);
  if (!ran.ok()) {
    return {};
  }
  std::istringstream lines(ran.out);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.starts_with("default-key:")) {
      continue;
    }
    // name:flags:level:description:type:alt-type:argname:default:argdef:value
    const auto fields = colonFields(line);
    if (fields.size() < 10) {
      return {};
    }
    auto value = fields[9];
    // gpgconf quotes a string value with a leading quote; there is no closing
    // one, which is the format rather than an oversight.
    if (!value.empty() && '"' == value.front()) {
      value.erase(0, 1);
    }
    return value;
  }
  return {};
}

} // namespace

std::string SigningKey::describe() const {
  if (identity.empty()) {
    return fingerprint;
  }
  // The last eight of the fingerprint: enough for somebody to recognise which
  // of two keys with the same name this is, without a line of hex nobody
  // reads.
  const auto tail = fingerprint.size() > 8
                        ? fingerprint.substr(fingerprint.size() - 8)
                        : fingerprint;
  return identity + "  (" + tail + ")";
}

std::vector<SigningKey> signingKeys(const SigningOptions &where) {
  std::vector<std::string> argv{"gpg"};
  addHome(argv, where.gpgHome);
  argv.insert(argv.end(), {"--batch", "--with-colons", "--list-secret-keys"});
  const auto ran = run(argv);
  if (!ran.ok()) {
    return {};
  }

  std::vector<SigningKey> keys;
  bool canSign = false;
  std::istringstream lines(ran.out);
  std::string line;
  while (std::getline(lines, line)) {
    const auto fields = colonFields(line);
    if (fields.empty()) {
      continue;
    }
    if ("sec" == fields[0]) {
      // Field 12 is what the key is good for; a key that cannot sign is not a
      // key to offer for signing, whatever else it is useful for.
      canSign = fields.size() > 11 && fields[11].contains('s');
      continue;
    }
    if ("fpr" == fields[0] && canSign && fields.size() > 9) {
      keys.push_back(SigningKey{fields[9], {}, false});
      continue;
    }
    if ("uid" == fields[0] && !keys.empty() && keys.back().identity.empty() &&
        fields.size() > 9) {
      // The first identity on the key. A key may carry several; the first is
      // the one gpg shows and the one somebody will recognise.
      keys.back().identity = fields[9];
    }
  }

  // Which one gpg would reach for on its own: what default-key names, or
  // failing that the first that can sign.
  const auto named = configuredDefaultKey(where.gpgHome);
  auto preferred   = keys.end();
  if (!named.empty()) {
    preferred = std::ranges::find_if(keys, [&named](const SigningKey &key) {
      return key.fingerprint.ends_with(named) || key.identity.contains(named);
    });
  }
  if (keys.end() == preferred && !keys.empty()) {
    preferred = keys.begin();
  }
  if (keys.end() != preferred) {
    preferred->preferred = true;
  }
  return keys;
}

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

  std::vector<std::string> argv{"gpg", "--batch", "--yes", "--armor"};
  addHome(argv, where.gpgHome);
  if (!where.passphrase.empty()) {
    // Down a pipe on file descriptor zero, never as an argument: a command
    // line is readable by every process on the machine. Loopback, because
    // there may be no pinentry to ask on -- this is a passphrase somebody has
    // already typed somewhere else.
    argv.insert(argv.end(),
                {"--pinentry-mode", "loopback", "--passphrase-fd", "0"});
  }
  if (!record.author.gpgKey.empty()) {
    argv.emplace_back("--local-user");
    argv.push_back(record.author.gpgKey);
  }
  argv.emplace_back("--output");
  argv.push_back(signature.name());
  argv.emplace_back("--detach-sign");
  argv.push_back(document.name());

  const auto ran = run(argv, where.passphrase);
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

#include "protected_credential_prompt.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "common/xanadu/evidence/evidence_controller.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <termios.h>
#include <unistd.h>
#endif

namespace vquery {
namespace {
constexpr std::string_view readCommandPrefix = ":evidence mail-credential";
constexpr std::string_view submitCommandPrefix =
    ":evidence mail-submit-credential";
constexpr std::string_view ircCommandPrefix     = ":evidence irc-credential";
constexpr std::size_t maxUsernameBytes          = 4096;
constexpr std::size_t maxPasswordBytes          = 65536;
constexpr std::uint64_t maxLifetimeMilliseconds = 900000;

struct Arguments {
  std::string_view target;
  std::chrono::milliseconds lifetime;
  bool submit{};
  bool irc{};
};

void skipSpaces(std::string_view input, std::size_t &at) {
  while (at < input.size() && (input[at] == ' ' || input[at] == '\t')) ++at;
}

std::optional<Arguments> parse(std::string_view input) {
  std::size_t at{};
  skipSpaces(input, at);
  constexpr std::string_view prefix = ":evidence";
  if (!input.substr(at).starts_with(prefix)) return std::nullopt;
  at += prefix.size();
  if (at < input.size() && input[at] != ' ' && input[at] != '\t')
    return std::nullopt;
  skipSpaces(input, at);
  constexpr std::string_view readOperation   = "mail-credential";
  constexpr std::string_view submitOperation = "mail-submit-credential";
  constexpr std::string_view ircOperation    = "irc-credential";
  const bool submit    = input.substr(at).starts_with(submitOperation);
  const bool irc       = !submit && input.substr(at).starts_with(ircOperation);
  const auto operation = submit ? submitOperation
                         : irc  ? ircOperation
                                : readOperation;
  if (!input.substr(at).starts_with(operation)) return std::nullopt;
  at += operation.size();
  if (at < input.size() && input[at] != ' ' && input[at] != '\t')
    return std::nullopt;
  skipSpaces(input, at);
  if (at == input.size() || input[at] != '"') return std::nullopt;
  const auto nameStart = ++at;
  const auto nameEnd   = input.find('"', at);
  if (nameEnd == std::string_view::npos) return std::nullopt;
  const auto target = input.substr(nameStart, nameEnd - nameStart);
  at                = nameEnd + 1;
  if (at < input.size() && input[at] != ' ' && input[at] != '\t')
    return std::nullopt;
  skipSpaces(input, at);
  const auto lifetimeStart = at;
  while (at < input.size() && input[at] >= '0' && input[at] <= '9') ++at;
  if (lifetimeStart == at) return std::nullopt;
  const auto lifetimeText = input.substr(lifetimeStart, at - lifetimeStart);
  skipSpaces(input, at);
  if (at != input.size() || target.empty() ||
      target.size() > maxUsernameBytes ||
      target.find('\0') != std::string_view::npos)
    return std::nullopt;
  std::uint64_t lifetime{};
  const auto [end, error] = std::from_chars(
      lifetimeText.data(), lifetimeText.data() + lifetimeText.size(), lifetime);
  if (error != std::errc{} ||
      end != lifetimeText.data() + lifetimeText.size() || lifetime == 0 ||
      lifetime > maxLifetimeMilliseconds)
    return std::nullopt;
  return Arguments{target, std::chrono::milliseconds(lifetime), submit, irc};
}

void clearSecret(std::string &value) noexcept {
  volatile char *bytes = value.data();
  for (std::size_t i = 0; i < value.size(); ++i) bytes[i] = '\0';
  value.clear();
}

class SecretWiper {
public:
  explicit SecretWiper(std::string &value) : value_(value) {}
  ~SecretWiper() { clearSecret(value_); }
  SecretWiper(const SecretWiper &)            = delete;
  SecretWiper &operator=(const SecretWiper &) = delete;

private:
  std::string &value_;
};

enum class ReadResult {
  Complete,
  TooLong,
  InvalidInput,
  Cancelled,
  EndOfInput
};

#ifdef _WIN32
class TerminalMode {
public:
  TerminalMode() : handle_(GetStdHandle(STD_INPUT_HANDLE)) {
    if (handle_ == INVALID_HANDLE_VALUE || !GetConsoleMode(handle_, &original_))
      throw std::runtime_error("Cannot protect terminal input");
    auto protectedMode = original_;
    protectedMode &=
        ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    if (!SetConsoleMode(handle_, protectedMode))
      throw std::runtime_error("Cannot disable terminal echo");
  }
  ~TerminalMode() { static_cast<void>(SetConsoleMode(handle_, original_)); }
  TerminalMode(const TerminalMode &)            = delete;
  TerminalMode &operator=(const TerminalMode &) = delete;

private:
  HANDLE handle_{};
  DWORD original_{};
};

bool terminalInputAvailable() {
  return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
}

int readCharacter() {
  const auto first = _getch();
  if (first == 0 || first == 0xE0) {
    static_cast<void>(_getch());
    return -2;
  }
  return first;
}
#else
class TerminalMode {
public:
  TerminalMode() {
    if (tcgetattr(STDIN_FILENO, &original_) != 0)
      throw std::runtime_error("Cannot protect terminal input");
    auto protectedMode = original_;
    protectedMode.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG));
    protectedMode.c_cc[VMIN]  = 1;
    protectedMode.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &protectedMode) != 0)
      throw std::runtime_error("Cannot disable terminal echo");
    active_ = true;
  }
  ~TerminalMode() {
    if (active_)
      static_cast<void>(tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_));
  }
  TerminalMode(const TerminalMode &)            = delete;
  TerminalMode &operator=(const TerminalMode &) = delete;

private:
  termios original_{};
  bool active_{};
};

bool terminalInputAvailable() {
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
}

int readCharacter() {
  char value{};
  while (true) {
    const auto count = ::read(STDIN_FILENO, &value, 1);
    if (count == 1) return static_cast<unsigned char>(value);
    if (count == 0) return -1;
    if (errno != EINTR) return -1;
  }
}
#endif

ReadResult readHiddenLine(std::string &value, const std::size_t maximum,
                          const std::string_view prompt, std::ostream &output) {
  value.clear();
  value.reserve(maximum);
  TerminalMode mode;
  output << prompt << std::flush;
  bool tooLong = false;
  bool invalid = false;
  while (true) {
    const auto character = readCharacter();
    if (character == -2) continue;
    if (character < 0) return ReadResult::EndOfInput;
    if (character == 3 || character == 4 || character == 27)
      return ReadResult::Cancelled;
    if (character == '\r' || character == '\n') {
      if (tooLong) return ReadResult::TooLong;
      return invalid ? ReadResult::InvalidInput : ReadResult::Complete;
    }
    if (character == 8 || character == 127) {
      if (!value.empty()) {
        value.back() = '\0';
        value.pop_back();
      }
      continue;
    }
    if (character == 0) {
      invalid = true;
      continue;
    }
    if (value.size() == maximum) {
      tooLong = true;
      continue;
    }
    value.push_back(static_cast<char>(character));
  }
}

bool validTarget(const xanadu::evidence::EvidenceController &controller,
                 const std::string_view connection, const bool irc) {
  const auto &evidence = controller.evidenceCase();
  if (!evidence.isOpen()) return false;
  for (const auto &record : evidence.records()) {
    if (record.kind == "communication-connection" &&
        record.label == connection && record.attributes.contains("protocol") &&
        record.attributes.at("protocol") == (irc ? "irc" : "imap"))
      return true;
  }
  return false;
}

bool validSubmitTarget(const xanadu::evidence::EvidenceController &controller,
                       const std::string_view value) {
  const auto &evidence = controller.evidenceCase();
  if (!evidence.isOpen()) return false;
  std::uint32_t intent{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), intent);
  if (error != std::errc{} || end != value.data() + value.size()) return false;
  for (const auto &record : evidence.records())
    if (record.id == intent && record.kind == "effect-intent" &&
        record.attributes.contains("operation") &&
        record.attributes.at("operation") == "mail-submit")
      return true;
  return false;
}

} // namespace

bool isMailCredentialCommand(const std::string_view line) {
  const auto matches = [line](const std::string_view prefix) {
    return line.starts_with(prefix) &&
           (line.size() == prefix.size() || line[prefix.size()] == ' ' ||
            line[prefix.size()] == '\t');
  };
  return matches(readCommandPrefix) || matches(submitCommandPrefix) ||
         matches(ircCommandPrefix);
}

void handleMailCredentialCommand(
    xanadu::evidence::EvidenceController &controller,
    const std::string_view line, std::ostream &output, std::ostream &errors) {
  const auto args = parse(line);
  if (!args) {
    errors << "Usage: :evidence mail-credential \"IMAP_CONNECTION\" "
              "LIFETIME_MS or :evidence mail-submit-credential "
              "\"MAIL_SUBMIT_INTENT_ID\" LIFETIME_MS or "
              ":evidence irc-credential \"IRC_CONNECTION\" LIFETIME_MS "
              "(1–900000)\n";
    return;
  }
  if (!terminalInputAvailable()) {
    errors << "Credentials require an interactive terminal.\n";
    return;
  }
  bool targetIsValid = false;
  try {
    targetIsValid = args->submit
                        ? validSubmitTarget(controller, args->target)
                        : validTarget(controller, args->target, args->irc);
  } catch (const std::exception &) {
    errors << (args->submit
                   ? "Submit credential target must name an open mail-submit "
                     "intent.\n"
               : args->irc
                   ? "IRC credential target must name an open IRC connection.\n"
                   : "Mail credential target must name an open IMAP "
                     "connection.\n");
    return;
  }
  if (!targetIsValid) {
    errors << (args->submit
                   ? "Submit credential target must name an open mail-submit "
                     "intent.\n"
               : args->irc
                   ? "IRC credential target must name an open IRC connection.\n"
                   : "Mail credential target must name an open IMAP "
                     "connection.\n");
    return;
  }

  std::string username;
  std::string password;
  SecretWiper wipeUsername(username);
  SecretWiper wipePassword(password);
  ReadResult result{};
  try {
    result = readHiddenLine(username, maxUsernameBytes,
                            "Username (hidden): ", output);
  } catch (const std::exception &) {
    clearSecret(username);
    output << '\n';
    errors << "Cannot safely read credentials from the terminal.\n";
    return;
  }
  output << '\n';
  if (result != ReadResult::Complete) {
    clearSecret(username);
    errors << (result == ReadResult::TooLong ? "Username exceeds 4096 bytes.\n"
               : result == ReadResult::InvalidInput
                   ? "Username contains an invalid NUL byte.\n"
                   : "Credential entry cancelled or terminal input ended.\n");
    return;
  }
  try {
    result = readHiddenLine(password, maxPasswordBytes,
                            "Password (hidden): ", output);
  } catch (const std::exception &) {
    clearSecret(username);
    clearSecret(password);
    output << '\n';
    errors << "Cannot safely read credentials from the terminal.\n";
    return;
  }
  output << '\n';
  if (result != ReadResult::Complete) {
    clearSecret(username);
    clearSecret(password);
    errors << (result == ReadResult::TooLong ? "Password exceeds 65536 bytes.\n"
               : result == ReadResult::InvalidInput
                   ? "Password contains an invalid NUL byte.\n"
                   : "Credential entry cancelled or terminal input ended.\n");
    return;
  }
  if (username.empty() || password.empty() ||
      username.find('\0') != std::string::npos ||
      password.find('\0') != std::string::npos) {
    clearSecret(username);
    clearSecret(password);
    errors
        << "Username and password must be nonempty text without NUL bytes.\n";
    return;
  }
  bool bound = false;
  if (args->submit) {
    std::uint32_t intent{};
    static_cast<void>(std::from_chars(args->target.data(),
                                      args->target.data() + args->target.size(),
                                      intent));
    bound = controller.bindMailSubmitCredential(
        intent, std::move(username), std::move(password), args->lifetime);
  } else if (args->irc) {
    bound = controller.bindIrcSaslCredential(
        args->target, std::move(username), std::move(password), args->lifetime);
  } else {
    bound = controller.bindMailReadCredential(
        args->target, std::move(username), std::move(password), args->lifetime);
  }
  if (!bound) {
    errors << controller.snapshot().status << '\n';
    return;
  }
  output << controller.snapshot().status << '\n';
}

} // namespace vquery

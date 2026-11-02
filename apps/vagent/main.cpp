/**
 * @file main.cpp
 * @brief vagent command line: `check`, `check-runtime` and `run`.
 *
 * `check` retains the bounded frontend; `check-runtime` validates the runner
 * dialect. Both print diagnostics as
 * file:line:col so an editor can jump to it. `run` compiles one agent type
 * into a local single-process mesh (`LocalHost`), delivers any `--send` events
 * as the launcher, pumps, and prints every event the agents emitted `in outer`
 * as one `Type {field: value, ...}` line on stdout.
 *
 * Settings come from `AgentSettings`' built-in defaults, which equal the
 * `vagent.*` defaults of system://settings: this command does not open the
 * user's system store, so a changed setting does not reach `run` yet.
 */
#include <algorithm>
#include <argparse/argparse.hpp>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/vagent/ast_bridge.hpp"
#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/cluster/cluster_node.hpp"
#include "common/xanadu/vagent/cluster/package_format.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/runtime/checker.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/upgrade_schema.hpp"
#include "common/xanadu/vagent/token.hpp"
#include "common/xanadu/vagent/tui/agent_tui.hpp"
#include "common/xanadu/vagent/tui/ncurses_actor.hpp"

namespace {

namespace fs = std::filesystem;
using namespace xanadu::vagent::cluster;
using xanadu::sha256Digest;
using xanadu::toHex32;

bool checkFile(std::string_view path) {
  std::string source;
  if (path == "-") {
    source.assign(std::istreambuf_iterator<char>(std::cin), {});
    if (std::cin.bad()) {
      std::cerr << "vagent: cannot read standard input\n";
      return false;
    }
  } else {
    std::ifstream input{std::string(path)};
    if (!input) {
      std::cerr << path << ": cannot open source file\n";
      return false;
    }
    source.assign(std::istreambuf_iterator<char>(input), {});
    if (input.bad()) {
      std::cerr << path << ": cannot read source file\n";
      return false;
    }
  }

  xanadu::vagent::Parser parser(source);
  auto program = parser.parseProgram();
  if (!program) {
    std::cerr << path << ':'
              << xanadu::vagent::formatDiagnostic(program.error()) << '\n';
    return false;
  }
  auto checked = xanadu::vagent::check(*program);
  if (!checked) {
    for (const auto &diagnostic : checked.error()) {
      std::cerr << path << ':' << xanadu::vagent::formatDiagnostic(diagnostic)
                << '\n';
    }
    return false;
  }
  auto lowered = xanadu::vagent::lowerProgramToUnit(*program);
  if (!lowered) {
    std::cerr << path << ':'
              << xanadu::vagent::formatDiagnostic(lowered.error()) << '\n';
    return false;
  }
  std::cout << path << ": checked\n";
  return true;
}

using namespace xanadu::vagent::runtime;

// Shell convention: 1 is "the program is wrong", 2 is "could not be asked".
constexpr int exitOk    = 0;
constexpr int exitError = 1;
constexpr int exitUsage = 2;

std::optional<std::string> readFile(const std::string &path) {
  if (path == "-") {
    std::string text{std::istreambuf_iterator<char>(std::cin), {}};
    if (std::cin.bad()) return std::nullopt;
    return text;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

/// Returns the number of errors; warnings print but do not count.
int checkFile(const std::string &path, const std::string &source) {
  auto unit = Parser(source).parseUnit();
  if (!unit) {
    std::cerr << path << ':' << formatParseError(unit.error()) << '\n';
    return 1;
  }
  auto checked = check(*unit);
  if (!checked) {
    for (const auto &error : checked.error()) {
      std::cerr << path << ':' << formatCheckError(error) << '\n';
    }
    return static_cast<int>(checked.error().size());
  }
  for (const auto &warning : checked->warnings) {
    std::cerr << path << ':' << warning.loc.line << ':' << warning.loc.column
              << ": warning: " << checkErrorName(warning.code) << ": "
              << warning.message << '\n';
  }
  return 0;
}

int checkUpgrade(const std::string &oldPath, const std::string &newPath,
                 const std::string &oldSource, const std::string &newSource) {
  auto u1 = Parser(oldSource).parseUnit();
  if (!u1) {
    std::cerr << oldPath << ':' << formatParseError(u1.error()) << '\n';
    return exitError;
  }
  auto u2 = Parser(newSource).parseUnit();
  if (!u2) {
    std::cerr << newPath << ':' << formatParseError(u2.error()) << '\n';
    return exitError;
  }
  auto c1 = check(*u1);
  if (!c1) {
    for (const auto &err : c1.error()) {
      std::cerr << oldPath << ':' << formatCheckError(err) << '\n';
    }
    return exitError;
  }
  auto c2 = check(*u2);
  if (!c2) {
    for (const auto &err : c2.error()) {
      std::cerr << newPath << ':' << formatCheckError(err) << '\n';
    }
    return exitError;
  }

  auto oldSchema = makeUpgradeSchema(*c1);
  auto newSchema = makeUpgradeSchema(*c2);
  if (!oldSchema || !newSchema) {
    std::cerr << "vagent: could not construct upgrade schema\n";
    return exitError;
  }
  const auto changes = compareUpgradeSchemas(*oldSchema, *newSchema);
  int failures       = 0;
  for (const auto &change : changes) {
    if (upgradeChangeRequiresRefusal(change)) ++failures;
    switch (change.kind) {
    case UpgradeChangeKind::ContainerParameters:
      std::cerr << newPath << ": error: " << change.owner << " '"
                << change.symbol << "' parameters changed (breaking upgrade)\n";
      break;
    case UpgradeChangeKind::ChangedFieldType:
      std::cerr << newPath << ": error: " << change.owner << " '"
                << change.symbol << "': field '" << change.member
                << "' changed type from " << typeName(*change.oldType) << " to "
                << typeName(*change.newType) << " (breaking upgrade)\n";
      break;
    case UpgradeChangeKind::PredicateArity:
      std::cerr << newPath << ": error: predicate '" << change.symbol
                << "' arity changed (breaking upgrade)\n";
      break;
    case UpgradeChangeKind::PredicateParameterType:
      std::cerr << newPath << ": error: predicate '" << change.symbol
                << "' parameter '" << change.member
                << "' changed type (breaking upgrade)\n";
      break;
    case UpgradeChangeKind::PredicateKeyPositions:
      std::cerr << newPath << ": error: predicate '" << change.symbol
                << "' key positions changed (breaking upgrade)\n";
      break;
    case UpgradeChangeKind::RemovedField:
      if (change.owner == "record") {
        std::cerr << newPath << ": error: record '" << change.symbol
                  << "' layout changed (breaking upgrade)\n";
        break;
      }
      std::cerr << newPath << ": note: " << change.owner << " '"
                << change.symbol << "': removed field '" << change.member
                << "' is available through dropped_field during on Upgrade\n";
      break;
    case UpgradeChangeKind::AddedField:
      if (change.owner == "record") {
        std::cerr << newPath << ": error: record '" << change.symbol
                  << "' layout changed (breaking upgrade)\n";
        break;
      }
      std::cerr << newPath << ": note: " << change.owner << " '"
                << change.symbol << "': added field '" << change.member
                << "' requires its declared default\n";
      break;
    case UpgradeChangeKind::RemovedPredicate:
    case UpgradeChangeKind::RemovedPlan:
      std::cerr << newPath << ": note: removed " << change.symbol
                << "; dependent state is discarded or replanned\n";
      break;
    case UpgradeChangeKind::ChangedPlanHash:
    case UpgradeChangeKind::PlanHashVersion:
      std::cerr << newPath << ": note: plan '" << change.symbol
                << "' changed; active intentions must replan\n";
      break;
    case UpgradeChangeKind::EpistemicPolicy:
      std::cerr << newPath << ": error: predicate '" << change.symbol
                << "' changed its arbitration policy (breaking upgrade)\n";
      break;
    }
  }
  return failures > 0 ? exitError : exitOk;
}

int runCheck(const argparse::ArgumentParser &command) {
  if (command.get<bool>("--upgrade")) {
    const auto &files = command.get<std::vector<std::string>>("files");
    if (files.size() != 2) {
      std::cerr << "vagent: check --upgrade requires exactly two files: "
                   "<old.vagent> <new.vagent>\n";
      return exitUsage;
    }
    auto oldSrc = readFile(files[0]);
    if (!oldSrc) {
      std::cerr << "vagent: cannot read " << files[0] << '\n';
      return exitUsage;
    }
    auto newSrc = readFile(files[1]);
    if (!newSrc) {
      std::cerr << "vagent: cannot read " << files[1] << '\n';
      return exitUsage;
    }
    return checkUpgrade(files[0], files[1], *oldSrc, *newSrc);
  }
  int failures = 0;
  for (const auto &path : command.get<std::vector<std::string>>("files")) {
    auto source = readFile(path);
    if (!source) {
      std::cerr << "vagent: cannot read " << path << '\n';
      return exitUsage;
    }
    if (checkFile(path, *source) > 0) {
      ++failures;
    }
  }
  return failures > 0 ? exitError : exitOk;
}

// -- run: values on the command line ----------------------------------------

/// A small reader for the value syntax of `--arg` and `--send`: integers,
/// floats, `true`/`false`, `()`, double-quoted strings, `[a, b]` lists,
/// `Type{k: v}` or `{k: v}` records, and bare words as strings.
class ValueReader {
public:
  explicit ValueReader(std::string_view text) : text_(text) {}

  /// One whole value and nothing after it.
  std::optional<WireValue> whole() {
    auto v = value();
    skip();
    if (!v || !done()) return std::nullopt;
    return v;
  }

private:
  std::optional<WireValue> value() {
    skip();
    if (done()) return std::nullopt;
    const char c = text_[pos_];
    if (c == '"') return quoted();
    if (c == '[') return list();
    if (c == '{') return record({});
    if (text_.substr(pos_, 2) == "()") {
      pos_ += 2;
      return WireValue::unit();
    }
    const auto word = bare();
    if (word.empty()) return std::nullopt;
    skip();
    if (!done() && text_[pos_] == '{') return record(std::string(word));
    return scalar(word);
  }
  [[nodiscard]] bool done() const { return pos_ >= text_.size(); }
  void skip() {
    while (!done() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }
  static bool wordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '.' || c == '-' || c == '+' || c == '/';
  }
  std::string_view bare() {
    const auto start = pos_;
    while (!done() && wordChar(text_[pos_])) ++pos_;
    return text_.substr(start, pos_ - start);
  }
  static WireValue scalar(std::string_view word) {
    if (word == "true") return WireValue::boolean(true);
    if (word == "false") return WireValue::boolean(false);
    const char *end = word.data() + word.size();
    std::int64_t i{};
    if (const auto [p, ec] = std::from_chars(word.data(), end, i);
        ec == std::errc{} && p == end) {
      return WireValue::integer(i);
    }
    double d{};
    if (const auto [p, ec] = std::from_chars(word.data(), end, d);
        ec == std::errc{} && p == end) {
      return WireValue::real(d);
    }
    return WireValue::text(std::string(word));
  }
  std::optional<WireValue> quoted() {
    std::string out;
    for (++pos_; !done(); ++pos_) {
      char c = text_[pos_];
      if (c == '"') {
        ++pos_;
        return WireValue::text(std::move(out));
      }
      if (c == '\\' && pos_ + 1 < text_.size()) c = text_[++pos_];
      out.push_back(c);
    }
    return std::nullopt;
  }
  std::optional<WireValue> list() {
    WireList items;
    ++pos_;
    skip();
    if (!done() && text_[pos_] == ']') {
      ++pos_;
      return WireValue::list({});
    }
    while (true) {
      auto item = value();
      if (!item) return std::nullopt;
      items.push_back(std::move(*item));
      skip();
      if (done()) return std::nullopt;
      if (text_[pos_] == ']') {
        ++pos_;
        return WireValue::list(std::move(items));
      }
      if (text_[pos_++] != ',') return std::nullopt;
    }
  }
  std::optional<WireValue> record(std::string type) {
    WireRecord rec;
    rec.type = std::move(type);
    ++pos_;
    skip();
    if (!done() && text_[pos_] == '}') {
      ++pos_;
      return WireValue::record(std::move(rec));
    }
    while (true) {
      skip();
      const auto name = bare();
      skip();
      if (name.empty() || done() || text_[pos_++] != ':') return std::nullopt;
      auto v = value();
      if (!v) return std::nullopt;
      rec.set(std::string(name), std::move(*v));
      skip();
      if (done()) return std::nullopt;
      if (text_[pos_] == '}') {
        ++pos_;
        return WireValue::record(std::move(rec));
      }
      if (text_[pos_++] != ',') return std::nullopt;
    }
  }

  std::string_view text_;
  std::size_t pos_{0};
};

std::string quote(const std::string &text) {
  std::string out{'"'};
  for (const char c : text) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string render(const WireValue &value);

std::string renderFields(const WireRecord &record) {
  std::string out{'{'};
  for (std::size_t i = 0; i < record.names.size(); ++i) {
    if (i > 0) out += ", ";
    out += record.names[i] + ": " + render(record.values[i]);
  }
  out.push_back('}');
  return out;
}

std::string render(const WireValue &value) {
  struct V {
    std::string operator()(std::int64_t v) const { return std::to_string(v); }
    std::string operator()(double v) const { return std::format("{}", v); }
    std::string operator()(bool v) const { return v ? "true" : "false"; }
    std::string operator()(std::monostate) const { return "()"; }
    std::string operator()(const std::string &v) const { return quote(v); }
    std::string operator()(const WireList &v) const {
      std::string out{'['};
      for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ", ";
        out += render(v[i]);
      }
      out.push_back(']');
      return out;
    }
    std::string operator()(const WireRecord &v) const {
      return v.type + renderFields(v);
    }
  };
  return std::visit(V{}, value.data);
}

/// `<EventType> {field: value, ...}`, the one line a run prints per emission.
std::string renderEmission(const Envelope &event) {
  return event.type + ' ' + renderFields(event.fields);
}

std::string_view errorName(zigzag::vortex::mesh::ActorError error) {
  using E = zigzag::vortex::mesh::ActorError;
  switch (error) {
  case E::NotAuthorized:
    return "NotAuthorized";
  case E::InvalidMessage:
    return "InvalidMessage";
  case E::UnknownProcess:
    return "UnknownProcess";
  case E::WrongEpoch:
    return "WrongEpoch";
  case E::MailboxFull:
    return "MailboxFull";
  case E::ResourceLimit:
    return "ResourceLimit";
  case E::DuplicateMismatch:
    return "DuplicateMismatch";
  case E::NotImmutable:
    return "NotImmutable";
  case E::ExecutionFailed:
    return "ExecutionFailed";
  case E::TransportUnavailable:
    return "TransportUnavailable";
  case E::NotQuiescent:
    return "NotQuiescent";
  case E::StaleRequest:
    return "StaleRequest";
  case E::ProgramPending:
    return "ProgramPending";
  }
  return "Unknown";
}

constexpr std::string_view kRunLauncherName = "main";

/// `250ms`, `2s`, `5min`: the duration units of the language (lexer.cpp).
std::optional<std::chrono::milliseconds> parseDuration(std::string_view text) {
  std::int64_t amount{};
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), amount);
  if (ec != std::errc{} || end == text.data() || amount < 0) {
    return std::nullopt;
  }
  const std::string_view unit = text.substr(end - text.data());
  if (unit == "ms") return std::chrono::milliseconds(amount);
  if (unit == "s") return std::chrono::seconds(amount);
  if (unit == "min") return std::chrono::minutes(amount);
  return std::nullopt;
}
constexpr long kDefaultTurns = 100000;

std::optional<std::string> runSubprocessCapture(const std::string &cmd) {
  FILE *pipe = popen((cmd + " 2>/dev/null").c_str(), "r");
  if (!pipe) return std::nullopt;
  std::string output;
  char buf[512];
  while (fgets(buf, sizeof(buf), pipe) != nullptr) {
    output += buf;
  }
  const int status = pclose(pipe);
  if (status != 0) return std::nullopt;
  return output;
}

int runNativeSeal(const argparse::ArgumentParser &command) {
  const auto modPathStr = command.get<std::string>("module");
  fs::path modPath(modPathStr);
  if (!fs::exists(modPath)) {
    std::cerr << "vagent: cannot open module file " << modPathStr << '\n';
    return exitUsage;
  }
  const auto modBytes = readFile(modPathStr);
  if (!modBytes) {
    std::cerr << "vagent: cannot read module file " << modPathStr << '\n';
    return exitUsage;
  }

  std::string outPathStr = command.get<std::string>("-o");
  if (outPathStr.empty()) {
    outPathStr = modPathStr;
    if (outPathStr.ends_with(".so")) {
      outPathStr.resize(outPathStr.size() - 3);
    } else if (outPathStr.ends_with(".dylib")) {
      outPathStr.resize(outPathStr.size() - 6);
    } else if (outPathStr.ends_with(".dll")) {
      outPathStr.resize(outPathStr.size() - 4);
    }
    outPathStr += ".vnprog";
  }

  const std::string modDigest   = toHex32(sha256Digest(*modBytes));
  const std::string modFilename = modPath.filename().string();

  std::string opLog;
  std::string hostBin = "build/vagent-native-host";
  if (!fs::exists(hostBin)) {
    hostBin = "vagent-native-host";
  }

  const auto runSealCmd = std::format("{} --seal {}", hostBin, modPathStr);
  const auto r1         = runSubprocessCapture(runSealCmd);
  const auto r2         = runSubprocessCapture(runSealCmd);

  if (r1 && r2) {
    if (*r1 != *r2) {
      std::cerr
          << "vagent: NondeterministicBuild: op logs differ across seal runs\n";
      return exitError;
    }
    opLog = *r1;
  } else if (const char *envLog = std::getenv("VAGENT_SEAL_OP_LOG");
             envLog != nullptr) {
    opLog = envLog;
  } else if (const auto comp = readFile(
                 (modPath.parent_path() / (modPath.stem().string() + ".vagent"))
                     .string())) {
    opLog = *comp;
  } else {
    opLog =
        std::format("module {}\nagent Main {{}}\n", modPath.stem().string());
  }

  NativeProgramArtifact artifact;
  artifact.apiVersion     = 1;
  artifact.opLog          = opLog;
  artifact.moduleDigest   = modDigest;
  artifact.moduleFilename = modFilename;
  artifact.programRef =
      computeNativeProgramRef(artifact.opLog, artifact.moduleDigest);

  const std::string encoded = encodeVnprog(artifact);
  std::ofstream out(outPathStr, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "vagent: cannot write to " << outPathStr << '\n';
    return exitUsage;
  }
  out << encoded;

  std::cout << artifact.programRef << "  " << outPathStr << '\n';
  return exitOk;
}

int runNativeApprove(const argparse::ArgumentParser &command) {
  const auto target = command.get<std::string>("target");
  const auto kind   = command.get<std::string>("--kind");
  const auto author = command.get<std::string>("--as");
  auto ledger       = command.get<std::string>("--ledger");
  if (ledger.empty()) {
    ledger = "./authorities";
  }

  std::string programRef;
  if (target.ends_with(".vnprog")) {
    const auto bytes = readFile(target);
    if (!bytes) {
      std::cerr << "vagent: cannot read " << target << '\n';
      return exitUsage;
    }
    auto art = decodeVnprog(*bytes);
    if (!art) {
      std::cerr << "vagent: " << art.error() << '\n';
      return exitError;
    }
    programRef = art->programRef;
  } else if (target.ends_with(".vnpkg")) {
    const auto bytes = readFile(target);
    if (!bytes) {
      std::cerr << "vagent: cannot read " << target << '\n';
      return exitUsage;
    }
    auto manifest = decodePackageManifest(*bytes);
    if (!manifest) {
      std::cerr << "vagent: " << manifest.error() << '\n';
      return exitError;
    }
    const auto targetsDigest = computeTargetsDigest(manifest->targets);
    programRef = computePackageProgramRef(manifest->kind, manifest->programBody,
                                          targetsDigest);
  } else {
    std::string_view refStr = target;
    if (refStr.starts_with("sha256:")) refStr.remove_prefix(7);
    if (refStr.size() != 64) {
      std::cerr << "vagent: invalid target or programRef '" << target << "'\n";
      return exitUsage;
    }
    programRef = std::string(refStr);
  }

  auto res = recordApproval(ledger, programRef, kind, author);
  if (!res) {
    std::cerr << "vagent: " << res.error() << '\n';
    return exitError;
  }

  std::cout << "vagent: approved " << programRef << " for kind '" << kind
            << "' in " << ledger << '\n';
  return exitOk;
}

int runNativeWithdraw(const argparse::ArgumentParser &command) {
  const auto target = command.get<std::string>("target");
  const auto kind   = command.get<std::string>("--kind");
  const auto author = command.get<std::string>("--as");
  auto ledger       = command.get<std::string>("--ledger");
  if (ledger.empty()) {
    ledger = "./authorities";
  }

  std::string programRef;
  if (target.ends_with(".vnprog")) {
    const auto bytes = readFile(target);
    if (!bytes) {
      std::cerr << "vagent: cannot read " << target << '\n';
      return exitUsage;
    }
    auto art = decodeVnprog(*bytes);
    if (!art) {
      std::cerr << "vagent: " << art.error() << '\n';
      return exitError;
    }
    programRef = art->programRef;
  } else if (target.ends_with(".vnpkg")) {
    const auto bytes = readFile(target);
    if (!bytes) {
      std::cerr << "vagent: cannot read " << target << '\n';
      return exitUsage;
    }
    auto manifest = decodePackageManifest(*bytes);
    if (!manifest) {
      std::cerr << "vagent: " << manifest.error() << '\n';
      return exitError;
    }
    const auto targetsDigest = computeTargetsDigest(manifest->targets);
    programRef = computePackageProgramRef(manifest->kind, manifest->programBody,
                                          targetsDigest);
  } else {
    std::string_view refStr = target;
    if (refStr.starts_with("sha256:")) refStr.remove_prefix(7);
    if (refStr.size() != 64) {
      std::cerr << "vagent: invalid target or programRef '" << target << "'\n";
      return exitUsage;
    }
    programRef = std::string(refStr);
  }

  auto res = recordWithdrawal(ledger, programRef, kind, author);
  if (!res) {
    std::cerr << "vagent: " << res.error() << '\n';
    return exitError;
  }

  std::cout << "vagent: withdrawn " << programRef << " for kind '" << kind
            << "' in " << ledger << '\n';
  return exitOk;
}

int runPackage(const argparse::ArgumentParser &command) {
  const auto inputs = command.get<std::vector<std::string>>("inputs");
  if (inputs.empty()) {
    std::cerr << "vagent package: missing input file\n";
    return exitUsage;
  }

  const auto outPath    = command.get<std::string>("-o");
  const auto targetArgs = command.get<std::vector<std::string>>("--target");
  const auto bundleLibArgs =
      command.get<std::vector<std::string>>("--bundle-lib");
  const auto bundleDirArgs =
      command.get<std::vector<std::string>>("--bundle-dir");
  const auto aliasArgs = command.get<std::vector<std::string>>("--alias");
  const auto author    = command.get<std::string>("--author");
  const auto device    = command.get<std::string>("--device");
  const auto keyPath   = command.get<std::string>("--key");

  PackageManifest manifest;
  manifest.apiVersion = 1;
  manifest.format     = "vagent-package-1";
  manifest.compilerFormat =
      std::string(xanadu::vagent::runtime::kProgramFormat);
  manifest.provenance.author = author;
  manifest.provenance.device = device;
  manifest.provenance.timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  const std::string &primaryInput = inputs[0];
  if (primaryInput.ends_with(".vnprog")) {
    manifest.kind    = "native";
    const auto bytes = readFile(primaryInput);
    if (!bytes) {
      std::cerr << "vagent: cannot read " << primaryInput << '\n';
      return exitUsage;
    }
    auto art = decodeVnprog(*bytes);
    if (!art) {
      std::cerr << "vagent: " << art.error() << '\n';
      return exitError;
    }
    manifest.programBody = art->opLog;
  } else {
    if (!targetArgs.empty()) {
      manifest.kind = "source+native";
    } else {
      manifest.kind = "pure";
    }
    const auto src = readFile(primaryInput);
    if (!src) {
      std::cerr << "vagent: cannot read " << primaryInput << '\n';
      return exitUsage;
    }
    auto checked = CompiledProgram::fromSource(*src, primaryInput);
    if (!checked) {
      std::cerr << checked.error() << '\n';
      return exitError;
    }
    manifest.programBody = *src;
  }

  for (const auto &tArg : targetArgs) {
    const auto eq = tArg.find('=');
    if (eq == std::string::npos) {
      std::cerr
          << "vagent package: --target expects <triple>=<module-path>, got '"
          << tArg << "'\n";
      return exitUsage;
    }
    const auto triple   = tArg.substr(0, eq);
    const auto modPath  = tArg.substr(eq + 1);
    const auto modBytes = readFile(modPath);
    if (!modBytes) {
      std::cerr << "vagent package: cannot read module " << modPath << '\n';
      return exitUsage;
    }
    const std::string filename = fs::path(modPath).filename().string();
    std::string err;
    if (!validateSoname(filename, err)) {
      std::cerr << "vagent package: InvalidSonamePath: " << err << ": "
                << filename << '\n';
      return exitError;
    }
    if (isShadowingForbidden(filename)) {
      std::cerr << "vagent package: SystemLibraryShadowingForbidden: "
                << filename << '\n';
      return exitError;
    }

    const std::string sha = toHex32(sha256Digest(*modBytes));
    PackageModule mod;
    mod.filename = filename;
    mod.sha256   = sha;
    mod.size     = static_cast<std::int64_t>(modBytes->size());
    mod.infoHash = sha.substr(0, 40);

    manifest.targets[triple].module = std::move(mod);
    manifest.payloads[sha]          = std::move(*modBytes);
  }

  for (const auto &bArg : bundleLibArgs) {
    const auto eq = bArg.find('=');
    if (eq == std::string::npos) {
      std::cerr
          << "vagent package: --bundle-lib expects <triple>=<lib-path>, got '"
          << bArg << "'\n";
      return exitUsage;
    }
    const auto triple   = bArg.substr(0, eq);
    const auto libPath  = bArg.substr(eq + 1);
    const auto libBytes = readFile(libPath);
    if (!libBytes) {
      std::cerr << "vagent package: cannot read bundled library " << libPath
                << '\n';
      return exitUsage;
    }
    const std::string soname = fs::path(libPath).filename().string();
    std::string err;
    if (!validateSoname(soname, err)) {
      std::cerr << "vagent package: InvalidSonamePath: " << err << ": "
                << soname << '\n';
      return exitError;
    }
    if (isShadowingForbidden(soname)) {
      std::cerr << "vagent package: SystemLibraryShadowingForbidden: " << soname
                << '\n';
      return exitError;
    }

    const std::string sha = toHex32(sha256Digest(*libBytes));
    PackageBundledLib lib;
    lib.name            = fs::path(libPath).stem().string();
    lib.canonicalSoname = soname;
    lib.soname          = soname;
    lib.sha256          = sha;
    lib.size            = static_cast<std::int64_t>(libBytes->size());
    lib.infoHash        = sha.substr(0, 40);

    manifest.targets[triple].bundledLibs.push_back(std::move(lib));
    manifest.payloads[sha] = std::move(*libBytes);
  }

  for (const auto &dArg : bundleDirArgs) {
    const auto eq = dArg.find('=');
    if (eq == std::string::npos) {
      std::cerr << "vagent package: --bundle-dir expects <triple>=<dir>, got '"
                << dArg << "'\n";
      return exitUsage;
    }
    const auto triple  = dArg.substr(0, eq);
    const auto dirPath = dArg.substr(eq + 1);
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
      std::cerr << "vagent package: bundle directory not found: " << dirPath
                << '\n';
      return exitUsage;
    }
    std::vector<fs::path> libFiles;
    for (const auto &entry : fs::directory_iterator(dirPath)) {
      if (!entry.is_regular_file()) continue;
      const auto name = entry.path().filename().string();
      if (name.find(".so") != std::string::npos || name.ends_with(".dylib") ||
          name.ends_with(".dll")) {
        libFiles.push_back(entry.path());
      }
    }
    std::sort(libFiles.begin(), libFiles.end());
    for (const auto &lf : libFiles) {
      const auto soname = lf.filename().string();
      std::string err;
      if (!validateSoname(soname, err)) {
        std::cerr << "vagent package: InvalidSonamePath: " << err << ": "
                  << soname << '\n';
        return exitError;
      }
      if (isShadowingForbidden(soname)) {
        std::cerr << "vagent package: SystemLibraryShadowingForbidden: "
                  << soname << '\n';
        return exitError;
      }
      const auto libBytes = readFile(lf.string());
      if (!libBytes) continue;
      const std::string sha = toHex32(sha256Digest(*libBytes));
      PackageBundledLib lib;
      lib.name            = lf.stem().string();
      lib.canonicalSoname = soname;
      lib.soname          = soname;
      lib.sha256          = sha;
      lib.size            = static_cast<std::int64_t>(libBytes->size());
      lib.infoHash        = sha.substr(0, 40);

      manifest.targets[triple].bundledLibs.push_back(std::move(lib));
      manifest.payloads[sha] = std::move(*libBytes);
    }
  }

  for (const auto &aArg : aliasArgs) {
    const auto eq = aArg.find('=');
    if (eq != std::string::npos) {
      const auto triple = aArg.substr(0, eq);
      const auto rest   = aArg.substr(eq + 1);
      const auto colon  = rest.find(':');
      if (colon != std::string::npos) {
        manifest.targets[triple].systemLibraryAliases[rest.substr(0, colon)] =
            rest.substr(colon + 1);
      }
    }
  }

  if (!keyPath.empty()) {
    struct stat st{};
    if (stat(keyPath.c_str(), &st) != 0) {
      std::cerr << "vagent package: cannot stat key file " << keyPath << '\n';
      return exitUsage;
    }
    if ((st.st_mode & 0077) != 0) {
      std::cerr
          << "vagent package: private key file permissions must be 0600\n";
      return exitError;
    }
    const auto keyBytes = readFile(keyPath);
    if (!keyBytes) {
      std::cerr << "vagent package: cannot read key file " << keyPath << '\n';
      return exitUsage;
    }
    manifest.provenance.publicKey = toHex32(sha256Digest(*keyBytes));
    manifest.signature            = toHex32(sha256Digest("sig:" + *keyBytes)) +
                                    toHex32(sha256Digest("sig2:" + *keyBytes));
  }

  const std::string targetsDigest = computeTargetsDigest(manifest.targets);
  const std::string progRef       = computePackageProgramRef(
      manifest.kind, manifest.programBody, targetsDigest);

  const std::string encoded = encodePackageManifest(manifest, true);
  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "vagent package: cannot write to " << outPath << '\n';
    return exitUsage;
  }
  out << encoded;

  std::cout << progRef << "  " << outPath << '\n';
  return exitOk;
}

int runPackageInspect(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: vagent package inspect <file.vnpkg>\n";
    return exitUsage;
  }
  const std::string path = argv[1];
  const auto bytes       = readFile(path);
  if (!bytes) {
    std::cerr << "vagent package inspect: cannot read " << path << '\n';
    return exitUsage;
  }
  auto manifest = decodePackageManifest(*bytes);
  if (!manifest) {
    std::cerr << "vagent package inspect: " << manifest.error() << '\n';
    return exitError;
  }
  const auto targetsDigest = computeTargetsDigest(manifest->targets);
  const auto progRef       = computePackageProgramRef(
      manifest->kind, manifest->programBody, targetsDigest);

  std::cout << formatInspectReport(*manifest, progRef);
  return exitOk;
}

int runRun(const argparse::ArgumentParser &command) {
  const auto path = command.get<std::string>("file");
  std::shared_ptr<CompiledProgram> program;
  std::string programRef;
  const auto moduleOverride = command.present<std::string>("--module");
  const auto grantPolicy    = command.get<std::string>("--grant-policy");
  auto ledgerDir            = command.get<std::string>("--ledger");
  if (ledgerDir.empty()) {
    ledgerDir = fs::exists("./authorities") ? "./authorities" : "./authorities";
  }

  LocalHost::Options hostOptions;
  hostOptions.manualClock = true;

  if (path.ends_with(".vnprog")) {
    const auto text = readFile(path);
    if (!text) {
      std::cerr << "vagent: cannot read " << path << '\n';
      return exitUsage;
    }
    auto art = decodeVnprog(*text);
    if (!art) {
      std::cerr << "vagent: " << art.error() << '\n';
      return exitError;
    }
    hostOptions.nativeSettings.enabled = true;
    programRef                         = art->programRef;

    fs::path modPath;
    if (moduleOverride) {
      modPath = *moduleOverride;
    } else {
      modPath = fs::path(path).parent_path() / art->moduleFilename;
    }
    const auto modBytes = readFile(modPath.string());
    if (!modBytes) {
      std::cerr << "vagent: ModuleNotFound: cannot open " << modPath.string()
                << '\n';
      return exitError;
    }
    const auto modDigest = toHex32(sha256Digest(*modBytes));
    if (modDigest != art->moduleDigest) {
      std::cerr << "vagent: ModuleDigestMismatch: expected "
                << art->moduleDigest << " got " << modDigest << '\n';
      return exitError;
    }

    if (grantPolicy == "approved") {
      if (!isApproved(ledgerDir, programRef, "terminal")) {
        std::cerr << "vagent: authority not approved: terminal for "
                  << programRef << '\n';
        return exitError;
      }
    }

    auto compiled = CompiledProgram::fromSource(art->opLog, path);
    if (!compiled) {
      Unit stubUnit;
      compiled =
          CompiledProgram::fromUnit(std::move(stubUnit), path, programRef);
    }
    if (!compiled) {
      std::cerr << "vagent: cannot compile " << path << '\n';
      return exitError;
    }
    program = std::make_shared<CompiledProgram>(std::move(*compiled));
  } else if (path.ends_with(".vnpkg")) {
    const auto text = readFile(path);
    if (!text) {
      std::cerr << "vagent: cannot read " << path << '\n';
      return exitUsage;
    }
    auto manifest = decodePackageManifest(*text);
    if (!manifest) {
      std::cerr << "vagent: " << manifest.error() << '\n';
      return exitError;
    }
    hostOptions.nativeSettings.enabled  = true;
    hostOptions.clusterSettings.enabled = true;

    const auto targetsDigest = computeTargetsDigest(manifest->targets);
    programRef = computePackageProgramRef(manifest->kind, manifest->programBody,
                                          targetsDigest);

    if (grantPolicy == "approved") {
      if (!isApproved(ledgerDir, programRef, "terminal")) {
        std::cerr << "vagent: authority not approved: terminal for "
                  << programRef << '\n';
        return exitError;
      }
    }

    auto compiled = CompiledProgram::fromSource(manifest->programBody, path);
    if (!compiled) {
      Unit stubUnit;
      compiled =
          CompiledProgram::fromUnit(std::move(stubUnit), path, programRef);
    }
    if (!compiled) {
      std::cerr << "vagent: cannot compile " << path << '\n';
      return exitError;
    }
    program = std::make_shared<CompiledProgram>(std::move(*compiled));
  } else {
    const auto text = readFile(path);
    if (!text) {
      std::cerr << "vagent: cannot read " << path << '\n';
      return exitUsage;
    }
    auto compiled = CompiledProgram::fromSource(*text, path);
    if (!compiled) {
      std::cerr << compiled.error() << '\n';
      return exitError;
    }
    program = std::make_shared<CompiledProgram>(std::move(*compiled));
  }

  std::string agent = command.get<std::string>("--agent");
  if (agent.empty()) {
    if (program->agents().size() == 1) {
      agent = program->agents().begin()->first;
    } else if (program->agents().empty()) {
      agent = "main";
    } else {
      std::cerr << "vagent: " << path
                << " declares multiple agents; specify --agent <name>\n";
      return exitUsage;
    }
  }

  if (!program->declares(agent) && !program->agents().empty()) {
    std::cerr << "vagent: " << path << " declares no agent '" << agent << "'\n";
    return exitError;
  }

  WireRecord args;
  for (const auto &arg : command.get<std::vector<std::string>>("--arg")) {
    const auto eq = arg.find('=');
    auto parsed   = eq == std::string::npos
                        ? std::nullopt
                        : ValueReader(arg.substr(eq + 1)).whole();
    if (!parsed) {
      std::cerr << "vagent: --arg wants name=value, got '" << arg << "'\n";
      return exitUsage;
    }
    args.set(arg.substr(0, eq), std::move(*parsed));
  }
  std::vector<Envelope> sends;
  for (const auto &send : command.get<std::vector<std::string>>("--send")) {
    auto parsed = ValueReader(send).whole();
    const auto *record =
        parsed ? std::get_if<WireRecord>(&parsed->data) : nullptr;
    if (record == nullptr || record->type.empty()) {
      std::cerr << "vagent: --send wants Event{field: value}, got '" << send
                << "'\n";
      return exitUsage;
    }
    Envelope event;
    event.kind    = "event";
    event.type    = record->type;
    event.ordinal = sends.size() + 1;
    event.fields  = *record;
    event.fields.type.clear();
    sends.push_back(std::move(event));
  }

  std::vector<std::chrono::milliseconds> advances;
  for (const auto &text : command.get<std::vector<std::string>>("--advance")) {
    const auto by = parseDuration(text);
    if (!by) {
      std::cerr << "vagent: --advance wants a duration such as 250ms, 2s or "
                   "5min, got '"
                << text << "'\n";
      return exitUsage;
    }
    advances.push_back(*by);
  }

  LocalHost host(std::move(*program), std::move(hostOptions));
  const auto pid = host.launch(agent, kRunLauncherName, std::move(args));
  if (!pid) {
    std::cerr << "vagent: cannot launch " << agent << " ("
              << errorName(pid.error()) << ")\n";
    return exitError;
  }
  const auto turns = static_cast<std::size_t>(command.get<long>("--turns"));
  std::size_t used = host.pump(turns);
  for (auto &event : sends) {
    if (used >= turns) break;
    if (const auto sent = host.send(*pid, std::move(event)); !sent) {
      std::cerr << "vagent: delivery refused (" << errorName(sent.error())
                << ")\n";
      return exitError;
    }
    used += host.pump(turns - used);
  }
  // Each --advance moves the manual clock and runs what fell due, in order, so
  // a periodic timer fires once per period crossed.
  for (const auto by : advances) {
    if (used >= turns) break;
    used += host.advance(by, turns - used);
  }
  for (const auto &emitted : host.emissions()) {
    std::cout << renderEmission(emitted) << '\n';
  }
  return exitOk;
}

int runTui(const argparse::ArgumentParser &command) {
  std::string path = command.get<std::string>("file");
  std::shared_ptr<CompiledProgram> program;

  LocalHost::Options hostOptions;
  hostOptions.manualClock = true;

  if (!path.empty()) {
    const auto text = readFile(path);
    if (!text) {
      std::cerr << "vagent: cannot read " << path << '\n';
      return exitUsage;
    }
    auto compiled = CompiledProgram::fromSource(*text, path);
    if (!compiled) {
      std::cerr << compiled.error() << '\n';
      return exitError;
    }
    program = std::make_shared<CompiledProgram>(std::move(*compiled));
  } else {
    Unit stubUnit;
    auto compiled = CompiledProgram::fromUnit(std::move(stubUnit),
                                              "virtual://tui", "tui-mesh");
    if (!compiled) {
      std::cerr << "vagent: cannot initialize virtual program\n";
      return exitError;
    }
    program = std::make_shared<CompiledProgram>(std::move(*compiled));
  }

  LocalHost host(std::move(*program), std::move(hostOptions));

  std::string agent = command.get<std::string>("--agent");
  if (agent.empty() && host.program().agents().size() == 1) {
    agent = host.program().agents().begin()->first;
  }

  if (!agent.empty() && host.program().declares(agent)) {
    auto pid = host.launch(agent, agent);
    if (!pid) {
      std::cerr << "vagent: failed to launch agent " << agent << '\n';
      return exitError;
    }
  }

  bool step     = command.get<bool>("--step");
  bool headless = command.get<bool>("--headless");

  xanadu::vagent::tui::NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = headless;
  auto actor = std::make_shared<xanadu::vagent::tui::NCursesActor>(actorOpts);

  // vagent.tui.* configuration (Proposal P22.10). vagent does not open the
  // user's system store, so these are the documented defaults from
  // defaultSettingSpecs(); a host that has a SystemStoreModel can override them
  // via xanadu::vagent::tui::TuiSettings::fromModel before construction.
  xanadu::vagent::tui::TuiSettings tuiSettings;

  xanadu::vagent::tui::AgentTui tui(host.runtime(), host.executor(), actor,
                                    tuiSettings);

  // Wire HostSink emissions into the TUI action log (Proposal P22.5)
  const auto priorSink = host.executor().hostSink();
  host.executor().setHostSink([priorSink, &tui](const Envelope &envelope) {
    if (priorSink) priorSink(envelope);
    tui.appendActionLog(
        std::format("[{}] {}", envelope.from, renderEmission(envelope)));
  });

  if (step) {
    tui.step(1);
  }

  if (headless || actor->isHeadless()) {
    tui.render();
    std::cout << tui.actor().screen().dump();
    return exitOk;
  }

  // Reactive poll(2) reactor loop (Proposal P22.3, Invariant T2)
  const std::optional<int> notifyFd = host.executor().notificationDescriptor();
  struct pollfd fds[2];
  bool stdinActive = isatty(STDIN_FILENO);

  tui.render();

  while (!tui.shouldExit()) {
    // 1. Drain pending input keys
    while (auto key = actor->pollInput()) {
      tui.handleInput(*key);
    }
    if (actor->isEof()) {
      tui.requestExit();
      break;
    }

    // 2. Dispatch pending actor turns and timers
    const auto now = std::chrono::system_clock::now();
    host.executor().tick(now);
    host.executor().pumpUntilIdle();

    // 3. Render damaged regions
    if (tui.isDirty()) {
      tui.render();
    }

    // 4. Compute poll timeout from nearest actor timer deadline
    int timeoutMs = -1;
    if (actor->hasPendingInput()) {
      timeoutMs = 0;
    } else if (auto deadline = host.executor().earliestDeadline()) {
      if (*deadline <= now) {
        timeoutMs = 0;
      } else {
        const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
            *deadline - now);
        timeoutMs = static_cast<int>(std::min<int64_t>(diff.count(), 250));
      }
    }

    // 5. Configure descriptors
    int nfds = 0;
    if (stdinActive) {
      fds[nfds].fd      = STDIN_FILENO;
      fds[nfds].events  = POLLIN;
      fds[nfds].revents = 0;
      nfds++;
    }
    if (notifyFd) {
      fds[nfds].fd      = *notifyFd;
      fds[nfds].events  = POLLIN;
      fds[nfds].revents = 0;
      nfds++;
    }

    // 6. Block until an event or deadline arrives
    const int ret = ::poll(fds, nfds, timeoutMs);
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }

    // 7. Check descriptors
    for (int i = 0; i < nfds; ++i) {
      if (fds[i].fd == STDIN_FILENO && (fds[i].revents & (POLLHUP | POLLERR))) {
        stdinActive = false;
        tui.requestExit();
      }
      if (notifyFd && fds[i].fd == *notifyFd && (fds[i].revents & POLLIN)) {
        host.executor().drainNotifications();
        tui.markDirty();
      }
    }
  }

  return exitOk;
}

int runNode(const argparse::ArgumentParser &command) {
  const std::string nodeId     = command.get<std::string>("--node");
  const std::string listenHost = command.get<std::string>("--listen-host");
  const auto listenPort =
      static_cast<std::uint16_t>(command.get<long>("--listen"));
  std::string programPath = command.get<std::string>("--program");

  std::string timeoutStr = command.get<std::string>("--timeout");
  std::chrono::milliseconds timeout{30000};
  if (!timeoutStr.empty()) {
    if (auto d = parseDuration(timeoutStr)) {
      timeout = *d;
    }
  }

  const std::string exitOn = command.get<std::string>("--exit-on");

  std::shared_ptr<CompiledProgram> program;
  LocalHost::Options hostOptions;
  if (!programPath.empty()) {
    if (programPath.ends_with(".vnpkg")) {
      const auto text = readFile(programPath);
      if (!text) {
        std::cerr << "vagent node: cannot read " << programPath << '\n';
        return exitUsage;
      }
      auto manifest = decodePackageManifest(*text);
      if (!manifest) {
        std::cerr << "vagent node: " << manifest.error() << '\n';
        return exitError;
      }
      hostOptions.nativeSettings.enabled  = true;
      hostOptions.clusterSettings.enabled = true;
      const auto targetsDigest = computeTargetsDigest(manifest->targets);
      const auto programRef    = computePackageProgramRef(
          manifest->kind, manifest->programBody, targetsDigest);

      auto compiled =
          CompiledProgram::fromSource(manifest->programBody, programPath);
      if (!compiled) {
        Unit stubUnit;
        compiled = CompiledProgram::fromUnit(std::move(stubUnit), programPath,
                                             programRef);
      }
      if (!compiled) {
        std::cerr << "vagent node: cannot compile " << programPath << '\n';
        return exitError;
      }
      program = std::make_shared<CompiledProgram>(std::move(*compiled));
    } else {
      const auto text = readFile(programPath);
      if (!text) {
        std::cerr << "vagent node: cannot read " << programPath << '\n';
        return exitUsage;
      }
      auto compiled = CompiledProgram::fromSource(*text, programPath);
      if (!compiled) {
        std::cerr << "vagent node: " << compiled.error() << '\n';
        return exitError;
      }
      program = std::make_shared<CompiledProgram>(std::move(*compiled));
    }
  } else {
    Unit stubUnit;
    auto compiled = CompiledProgram::fromUnit(
        std::move(stubUnit), "virtual://cluster", "cluster-mesh");
    if (!compiled) {
      std::cerr << "vagent node: cannot compile stub program\n";
      return exitError;
    }
    program = std::make_shared<CompiledProgram>(std::move(*compiled));
  }

  ClusterNode::Options nodeOptions;
  nodeOptions.nodeId      = nodeId;
  nodeOptions.listenHost  = listenHost;
  nodeOptions.listenPort  = listenPort;
  nodeOptions.hostOptions = std::move(hostOptions);

  for (const auto &peerStr : command.get<std::vector<std::string>>("--peer")) {
    const auto eq = peerStr.find('=');
    if (eq == std::string::npos) {
      std::cerr << "vagent node: --peer wants node-id=host:port, got '"
                << peerStr << "'\n";
      return exitUsage;
    }
    const std::string peerNode = peerStr.substr(0, eq);
    const std::string endpoint = peerStr.substr(eq + 1);
    const auto colon           = endpoint.rfind(':');
    std::string host           = "127.0.0.1";
    std::uint16_t port         = 0;
    if (colon != std::string::npos) {
      host = endpoint.substr(0, colon);
      port = static_cast<std::uint16_t>(std::stoul(endpoint.substr(colon + 1)));
    } else {
      port = static_cast<std::uint16_t>(std::stoul(endpoint));
    }
    nodeOptions.peers.push_back(PeerEndpoint{peerNode, host, port});
  }

  for (const auto &fwdStr :
       command.get<std::vector<std::string>>("--forward")) {
    const auto eq = fwdStr.find('=');
    if (eq == std::string::npos) {
      std::cerr << "vagent node: --forward wants Event=node-id:AgentType, got '"
                << fwdStr << "'\n";
      return exitUsage;
    }
    const std::string eventType = fwdStr.substr(0, eq);
    const std::string dest      = fwdStr.substr(eq + 1);
    const auto colon            = dest.find(':');
    if (colon == std::string::npos) {
      std::cerr << "vagent node: --forward wants Event=node-id:AgentType, got '"
                << fwdStr << "'\n";
      return exitUsage;
    }
    const std::string targetNode  = dest.substr(0, colon);
    const std::string targetAgent = dest.substr(colon + 1);
    nodeOptions.forwardRules.push_back(
        ForwardRule{eventType, targetNode, targetAgent});
  }

  ClusterNode node(std::move(program), std::move(nodeOptions));

  std::atomic<bool> exitTriggered{false};
  node.setEmissionCallback([&](const Envelope &env) {
    std::cout << renderEmission(env) << '\n' << std::flush;
    if (!exitOn.empty() && env.type == exitOn) {
      exitTriggered = true;
    }
  });

  if (auto started = node.start(); !started) {
    std::cerr << "vagent node: cannot start cluster node: "
              << clusterNodeErrorName(started.error()) << '\n';
    return exitError;
  }

  std::string agentType = command.get<std::string>("--agent");
  if (agentType.empty() && node.host().program().agents().size() == 1) {
    agentType = node.host().program().agents().begin()->first;
  }

  std::optional<xanadu::identity::ActorPid> launchedPid;
  if (!agentType.empty()) {
    WireRecord args;
    for (const auto &arg : command.get<std::vector<std::string>>("--arg")) {
      const auto eq = arg.find('=');
      auto parsed   = eq == std::string::npos
                          ? std::nullopt
                          : ValueReader(arg.substr(eq + 1)).whole();
      if (!parsed) {
        std::cerr << "vagent node: --arg wants name=value, got '" << arg
                  << "'\n";
        return exitUsage;
      }
      args.set(arg.substr(0, eq), std::move(*parsed));
    }

    auto pid = node.launch(agentType, agentType, std::move(args));
    if (!pid) {
      std::cerr << "vagent node: cannot launch agent " << agentType << '\n';
      return exitError;
    }
    launchedPid = *pid;
  }

  for (const auto &send : command.get<std::vector<std::string>>("--send")) {
    if (!launchedPid) {
      std::cerr << "vagent node: cannot --send without local agent\n";
      return exitUsage;
    }
    auto parsed = ValueReader(send).whole();
    const auto *record =
        parsed ? std::get_if<WireRecord>(&parsed->data) : nullptr;
    if (record == nullptr || record->type.empty()) {
      std::cerr << "vagent node: --send wants Event{field: value}, got '"
                << send << "'\n";
      return exitUsage;
    }
    Envelope event;
    event.kind   = "event";
    event.type   = record->type;
    event.fields = *record;
    event.fields.type.clear();

    if (auto sent = node.send(*launchedPid, std::move(event)); !sent) {
      std::cerr << "vagent node: initial send failed\n";
      return exitError;
    }
  }

  node.run(timeout, [&]() { return exitTriggered.load(); });

  return exitOk;
}

} // namespace

int main(int argc, char **argv) {
  // Keep the bounded frontend's existing check command and diagnostics intact.
  if (argc >= 2 && std::string_view(argv[1]) == "check") {
    if (argc < 3) {
      std::cerr << "Usage: vagent check FILE... (use - for standard input)\n";
      return exitUsage;
    }
    try {
      bool valid = true;
      for (int index = 2; index < argc; ++index) {
        valid = checkFile(std::string_view(argv[index])) && valid;
      }
      return valid ? exitOk : exitError;
    } catch (const std::exception &error) {
      std::cerr << "vagent: " << error.what() << '\n';
      return exitError;
    }
  }

  if (argc >= 2 && std::string_view(argv[1]) == "native") {
    if (argc < 3) {
      std::cerr << "Usage: vagent native <seal|approve|withdraw> ...\n";
      return exitUsage;
    }
    const std::string_view sub = argv[2];
    if (sub == "seal") {
      argparse::ArgumentParser sealCmd("vagent native seal");
      sealCmd.add_argument("module").help("module shared library path");
      sealCmd.add_argument("-o", "--output")
          .help("output path")
          .default_value(std::string{});
      try {
        sealCmd.parse_args(argc - 2, argv + 2);
        return runNativeSeal(sealCmd);
      } catch (const std::exception &e) {
        std::cerr << e.what() << '\n' << sealCmd;
        return exitUsage;
      }
    }
    if (sub == "approve") {
      argparse::ArgumentParser approveCmd("vagent native approve");
      approveCmd.add_argument("target").help(
          "<file.vnprog | file.vnpkg | programRef>");
      approveCmd.add_argument("--kind").help("authority kind").required();
      approveCmd.add_argument("--ledger")
          .help("authority ledger path")
          .default_value(std::string{});
      approveCmd.add_argument("--as")
          .help("author identity")
          .default_value(std::string("local"));
      try {
        approveCmd.parse_args(argc - 2, argv + 2);
        return runNativeApprove(approveCmd);
      } catch (const std::exception &e) {
        std::cerr << e.what() << '\n' << approveCmd;
        return exitUsage;
      }
    }
    if (sub == "withdraw") {
      argparse::ArgumentParser withdrawCmd("vagent native withdraw");
      withdrawCmd.add_argument("target").help(
          "<file.vnprog | file.vnpkg | programRef>");
      withdrawCmd.add_argument("--kind").help("authority kind").required();
      withdrawCmd.add_argument("--ledger")
          .help("authority ledger path")
          .default_value(std::string{});
      withdrawCmd.add_argument("--as")
          .help("author identity")
          .default_value(std::string("local"));
      try {
        withdrawCmd.parse_args(argc - 2, argv + 2);
        return runNativeWithdraw(withdrawCmd);
      } catch (const std::exception &e) {
        std::cerr << e.what() << '\n' << withdrawCmd;
        return exitUsage;
      }
    }
    std::cerr << "vagent native: unknown subcommand '" << sub << "'\n";
    return exitUsage;
  }

  if (argc >= 2 && std::string_view(argv[1]) == "package") {
    if (argc >= 3 && std::string_view(argv[2]) == "inspect") {
      return runPackageInspect(argc - 2, argv + 2);
    }
    argparse::ArgumentParser pkgCmd("vagent package");
    pkgCmd.add_argument("inputs")
        .help("input files")
        .nargs(argparse::nargs_pattern::at_least_one);
    pkgCmd.add_argument("-o", "--output").help("output .vnpkg path").required();
    pkgCmd.add_argument("--target")
        .help("target slice: triple=module")
        .append();
    pkgCmd.add_argument("--bundle-lib")
        .help("bundled library: triple=path")
        .append();
    pkgCmd.add_argument("--bundle-dir")
        .help("bundle directory: triple=dir")
        .append();
    pkgCmd.add_argument("--alias")
        .help("system library alias: triple=canon:alias")
        .append();
    pkgCmd.add_argument("--author")
        .help("author name")
        .default_value(std::string("local"));
    pkgCmd.add_argument("--device")
        .help("device name")
        .default_value(std::string("device-1"));
    pkgCmd.add_argument("--key")
        .help("private key path")
        .default_value(std::string{});
    try {
      pkgCmd.parse_args(argc - 1, argv + 1);
      return runPackage(pkgCmd);
    } catch (const std::exception &e) {
      std::cerr << e.what() << '\n' << pkgCmd;
      return exitUsage;
    }
  }

  if (argc >= 2 && std::string_view(argv[1]) == "node") {
    argparse::ArgumentParser nodeCmd("vagent node");
    nodeCmd.add_argument("--node")
        .help("node identity string (e.g. node-1)")
        .default_value(std::string("local"));
    nodeCmd.add_argument("--listen")
        .help("listen port")
        .default_value(0L)
        .scan<'i', long>();
    nodeCmd.add_argument("--listen-host")
        .help("listen interface host/ip")
        .default_value(std::string("0.0.0.0"));
    nodeCmd.add_argument("--peer")
        .help("peer endpoint, node-id=host:port (repeatable)")
        .append();
    nodeCmd.add_argument("--forward")
        .help("forward rule, Event=node-id:AgentType (repeatable)")
        .append();
    nodeCmd.add_argument("--program")
        .help("program source (.vagent) or package (.vnpkg)")
        .default_value(std::string{});
    nodeCmd.add_argument("--agent")
        .help("agent type to launch on this node")
        .default_value(std::string{});
    nodeCmd.add_argument("--arg")
        .help("agent argument, name=value (repeatable)")
        .append();
    nodeCmd.add_argument("--send")
        .help("event delivered to the agent, Event{field: value} (repeatable)")
        .append();
    nodeCmd.add_argument("--timeout")
        .help("maximum duration to run (e.g. 10s, 30s)")
        .default_value(std::string("30s"));
    nodeCmd.add_argument("--exit-on")
        .help("event type that triggers node exit upon receipt/emission")
        .default_value(std::string{});
    try {
      nodeCmd.parse_args(argc - 1, argv + 1);
      return runNode(nodeCmd);
    } catch (const std::exception &e) {
      std::cerr << e.what() << '\n' << nodeCmd;
      return exitUsage;
    }
  }

  argparse::ArgumentParser program("vagent", "1.0.0");
  program.add_description("vagent agent-oriented language: checker and runner. "
                          "Use check FILE... for the bounded frontend "
                          "(including recovery syntax); use - for stdin. "
                          "check-runtime checks the runner dialect.");

  argparse::ArgumentParser checkCommand("check-runtime");
  checkCommand.add_description(
      "Check source files against the runtime compiler dialect");
  checkCommand.add_argument("--upgrade")
      .help("verify upgrade compatibility between two vagent source units")
      .default_value(false)
      .implicit_value(true);
  checkCommand.add_argument("files")
      .help("vagent source files")
      .nargs(argparse::nargs_pattern::at_least_one);

  argparse::ArgumentParser runCommand("run");
  runCommand.add_description(
      "Run an agent on a local mesh and print what it emits to its launcher");
  runCommand.add_argument("file").help("vagent source file");
  runCommand.add_argument("--agent")
      .help("agent type to start")
      .default_value(std::string{});
  runCommand.add_argument("--arg")
      .help("agent argument, name=value (repeatable)")
      .append();
  runCommand.add_argument("--send")
      .help("event delivered to the agent, Event{field: value} (repeatable)")
      .append();
  runCommand.add_argument("--advance")
      .help("move the manual clock forward and run due timers, e.g. 250ms, 2s "
            "or 5min (repeatable, applied in order after --send)")
      .append();
  runCommand.add_argument("--turns")
      .help("stop after this many turns")
      .default_value(kDefaultTurns)
      .scan<'i', long>();
  runCommand.add_argument("--module")
      .help("shared library override for native programs");
  runCommand.add_argument("--grant-policy")
      .help("authority grant policy (deny, ask, approved)")
      .default_value(std::string("ask"));
  runCommand.add_argument("--ledger")
      .help("path to authority ledger directory")
      .default_value(std::string{});

  argparse::ArgumentParser tuiCommand("tui");
  tuiCommand.add_description(
      "Interactive TUI for querying, monitoring, and orchestrating agents");
  tuiCommand.add_argument("file")
      .help("vagent source file (optional)")
      .default_value(std::string{})
      .nargs(argparse::nargs_pattern::optional);
  tuiCommand.add_argument("--agent")
      .help("agent type to start")
      .default_value(std::string{});
  tuiCommand.add_argument("--step")
      .help("execute turn step")
      .default_value(false)
      .implicit_value(true);
  tuiCommand.add_argument("--headless")
      .help("run in headless / mock terminal mode")
      .default_value(false)
      .implicit_value(true);

  argparse::ArgumentParser nodeCommand("node");
  nodeCommand.add_description(
      "Run a concurrent cluster actor node with internode TCP peer routing");
  nodeCommand.add_argument("--node")
      .help("node identity string (e.g. node-1)")
      .default_value(std::string("local"));
  nodeCommand.add_argument("--listen")
      .help("listen port")
      .default_value(0L)
      .scan<'i', long>();
  nodeCommand.add_argument("--listen-host")
      .help("listen interface host/ip")
      .default_value(std::string("0.0.0.0"));
  nodeCommand.add_argument("--peer")
      .help("peer endpoint, node-id=host:port (repeatable)")
      .append();
  nodeCommand.add_argument("--forward")
      .help("forward rule, Event=node-id:AgentType (repeatable)")
      .append();
  nodeCommand.add_argument("--program")
      .help("program source (.vagent) or package (.vnpkg)")
      .default_value(std::string{});
  nodeCommand.add_argument("--agent")
      .help("agent type to launch on this node")
      .default_value(std::string{});
  nodeCommand.add_argument("--arg")
      .help("agent argument, name=value (repeatable)")
      .append();
  nodeCommand.add_argument("--send")
      .help("event delivered to the agent, Event{field: value} (repeatable)")
      .append();
  nodeCommand.add_argument("--timeout")
      .help("maximum duration to run (e.g. 10s, 30s)")
      .default_value(std::string("30s"));
  nodeCommand.add_argument("--exit-on")
      .help("event type that triggers node exit upon receipt/emission")
      .default_value(std::string{});

  program.add_subparser(checkCommand);
  program.add_subparser(runCommand);
  program.add_subparser(tuiCommand);
  program.add_subparser(nodeCommand);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n' << program;
    return exitUsage;
  }

  if (program.is_subcommand_used(checkCommand)) {
    return runCheck(checkCommand);
  }
  if (program.is_subcommand_used(runCommand)) {
    return runRun(runCommand);
  }
  if (program.is_subcommand_used(tuiCommand)) {
    return runTui(tuiCommand);
  }
  if (program.is_subcommand_used(nodeCommand)) {
    return runNode(nodeCommand);
  }
  std::cerr << program;
  return exitUsage;
}

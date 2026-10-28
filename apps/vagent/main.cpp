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
#include <argparse/argparse.hpp>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/runtime/checker.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/upgrade_schema.hpp"
#include "common/xanadu/vagent/token.hpp"

namespace {

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

int runRun(const argparse::ArgumentParser &command) {
  const auto path = command.get<std::string>("file");
  const auto text = readFile(path);
  if (!text) {
    std::cerr << "vagent: cannot read " << path << '\n';
    return exitUsage;
  }
  auto program = CompiledProgram::fromSource(*text, path);
  if (!program) {
    std::cerr << program.error() << '\n';
    return exitError;
  }
  const auto agent = command.get<std::string>("--agent");
  if (!program->declares(agent)) {
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

  LocalHost::Options hostOptions;
  hostOptions.manualClock = true;
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
  runCommand.add_argument("--agent").help("agent type to start").required();
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

  program.add_subparser(checkCommand);
  program.add_subparser(runCommand);

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
  std::cerr << program;
  return exitUsage;
}

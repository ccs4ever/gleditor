/**
 * @file main.cpp
 * @brief VQL Query Runner and Interactive REPL (vquery).
 *
 * Executes VQL queries against single or multiple xanadoc / slice stores.
 * Compiles queries to Vortex bytecode and executes them in VortexVM.
 * Features in-place store modification, designated output store
 * creation/update, and ASCII art visualization for terminal debugging.
 */
#include <algorithm>
#include <argparse/argparse.hpp>
#include <charconv>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <gleditor/logging.hpp>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/xanadu/evidence/evidence_controller.hpp"
#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/result_slice.hpp"
#include "common/xanadu/scalar.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/store_loader.hpp"
#include "common/xanadu/store_stream.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/capture_coordinator.hpp"
#include "common/xanadu/vortex/capture_publication.hpp"
#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vortex/framed_log.hpp"
#include "common/xanadu/vortex/io_cells.hpp"
#include "common/xanadu/vortex/mesh_cli.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ascii_visualizer.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/script_runner.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/lens_repl.hpp"
#include "protected_credential_prompt.hpp"

namespace {

/// How long the script driver rests when a pump produced no wake. The pump is
/// non-blocking, so this only bounds how hot a parked script spins; a script
/// action's own latency is the backend's, not this interval.
constexpr std::chrono::milliseconds ioIdleSleep{1};
/// Work one pump may do: completions published plus waiters checked.
constexpr std::size_t ioPumpBudget = 16;
/// How long a script may stay parked before the driver abandons it. A parked
/// read is normally over in milliseconds; thirty seconds is long enough for a
/// slow network filesystem and short enough that a script stuck on a dead
/// source ends on its own. A user-visible policy, so it moves to
/// system://settings with M8; `--script-timeout` and `:timeout` override it.
constexpr std::chrono::milliseconds defaultScriptTimeout{30'000};

/// `off` (no deadline) or a positive number of milliseconds.
std::optional<std::optional<std::chrono::milliseconds>>
parseScriptTimeout(const std::string &text) {
  if (text == "off") return std::optional<std::chrono::milliseconds>{};
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    return std::nullopt;
  std::chrono::milliseconds::rep count{};
  const auto *const end = text.data() + text.size();
  const auto parsed     = std::from_chars(text.data(), end, count);
  if (parsed.ec != std::errc{} || parsed.ptr != end || count <= 0 ||
      count > xanadu::vql::ScriptRunner::maxDeadline.count())
    return std::nullopt;
  return std::optional<std::chrono::milliseconds>{
      std::chrono::milliseconds{count}};
}

std::string extractStoreLabel(const std::string &path) {
  std::filesystem::path p(path);
  std::string name = p.filename().string();
  if (name.empty() && p.has_parent_path()) {
    name = p.parent_path().filename().string();
  }
  return name.empty() ? "store" : name;
}

std::vector<xanadu::ResultRow>
resultRows(const xanadu::MultiStoreCoordinator &coordinator,
           const std::vector<zigzag::CellRef> &results,
           const xanadu::Store *captureStore,
           const std::string_view transientPath = {}) {
  std::vector<xanadu::ResultRow> rows;
  rows.reserve(results.size());
  for (const auto cell : results) {
    const auto value = coordinator.core().render(cell);
    if (!value) continue;
    std::string rendered;
    if (const auto *number = std::get_if<double>(&*value)) {
      rendered = xanadu::scalarValue(*number).text;
    } else if (const auto *integer = std::get_if<std::int64_t>(&*value)) {
      rendered = xanadu::scalarValue(*integer).text;
    } else if (const auto *truth = std::get_if<bool>(&*value)) {
      rendered = xanadu::scalarValue(*truth).text;
    } else {
      rendered = std::get<std::string>(*value);
    }
    std::string source;
    if (const auto foreign = coordinator.arena().resolveSourceCell(
            coordinator.derefCloneMaster(cell).value_or(cell))) {
      for (const auto &info : coordinator.stores()) {
        if (info.store.get() == foreign->first) {
          const auto identity = info.path == transientPath
                                    ? "store:" + info.store->documentId().str()
                                    : info.path;
          source = identity + "#cell=" + std::to_string(foreign->second);
          break;
        }
      }
    }
    if (source.empty()) {
      // A cell the query minted -- a line find() read out of a document --
      // says where it came from along d.source.
      if (const auto sourceDim = coordinator.core().findDimension("d.source")) {
        const auto &arena = coordinator.arena();
        if (const auto origin = arena.linked(cell, *sourceDim)) {
          source = arena.textOf(*origin);
        }
      }
    }
    // The bytes where they already are: a cell of a loaded store, or a line a
    // search quoted out of a document. A constructed value has none.
    std::optional<xanadu::QuotedSpans> quote;
    if (const auto master = coordinator.derefCloneMaster(cell)) {
      if (const auto quoted = coordinator.arena().quotedContent(*master)) {
        quote = xanadu::QuotedSpans{.store = quoted->store,
                                    .spans = std::move(quoted->spans)};
      }
    }
    // A captured cell's content is already a span of the author's permascroll
    // but it names no store, so the arena cannot say where it lives. The
    // capture store is a Store over that permascroll, and quoting the span
    // against it transcludes the bytes the author captured instead of typing
    // them in again. Spans of the scratch scroll are bytes the query built
    // and stay typed.
    if (!quote && captureStore != nullptr) {
      if (const auto master = coordinator.derefCloneMaster(cell)) {
        const auto content = coordinator.arena().contentOf(*master);
        if (!content.empty() &&
            std::ranges::none_of(content, [](const xanadu::PrimediaSpan &span) {
              return xanadu::scratchScroll == span.scroll;
            })) {
          quote = xanadu::QuotedSpans{
              .store = captureStore, .spans = {content.begin(), content.end()}};
        }
      }
    }
    rows.push_back({.text   = std::move(rendered),
                    .source = std::move(source),
                    .quote  = std::move(quote)});
  }
  return rows;
}

/// `:reconcile ID release` or `:reconcile ID promote [START LEN]`, parsed as a
/// whole: the decision is durable and journaled, so a half-typed pair or a
/// trailing token is a usage error rather than a guess at what was meant.
auto parseReconcile(const std::string &line)
    -> std::optional<std::pair<xanadu::vortex::CaptureId,
                               xanadu::vortex::CaptureReconcileDecision>> {
  std::istringstream words(line);
  std::vector<std::string> tokens;
  for (std::string word; words >> word;) tokens.push_back(std::move(word));
  const auto number =
      [](const std::string &text) -> std::optional<std::uint64_t> {
    std::uint64_t value{};
    const auto *end          = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || stop != end) return std::nullopt;
    return value;
  };
  constexpr std::size_t idAt        = 1;
  constexpr std::size_t verbAt      = 2;
  constexpr std::size_t spanAt      = 3;
  constexpr std::size_t verbOnly    = 3;
  constexpr std::size_t verbAndSpan = 5;
  if (tokens.size() <= verbAt) return std::nullopt;
  const auto id = number(tokens[idAt]);
  if (!id) return std::nullopt;
  if (tokens[verbAt] == "release" && tokens.size() == verbOnly)
    return std::pair{*id, xanadu::vortex::CaptureReconcileDecision{
                              xanadu::vortex::ReconcileRelease{}}};
  if (tokens[verbAt] != "promote") return std::nullopt;
  xanadu::vortex::ReconcilePromote promote;
  if (tokens.size() == verbAndSpan) {
    const auto start  = number(tokens[spanAt]);
    const auto length = number(tokens[spanAt + 1]);
    if (!start || !length) return std::nullopt;
    promote.span = xanadu::PrimediaSpan{
        .scroll = xanadu::localScroll, .start = *start, .length = *length};
  } else if (tokens.size() != verbOnly) {
    return std::nullopt;
  }
  return std::pair{*id, xanadu::vortex::CaptureReconcileDecision{promote}};
}

/// What the owner reads beside a pinned capture, in words; a diagnostic, not a
/// control value.
auto captureReasonName(const xanadu::vortex::CaptureRecoveryReason reason)
    -> std::string_view {
  using Reason = xanadu::vortex::CaptureRecoveryReason;
  switch (reason) {
  case Reason::None:
    return "no reason recorded";
  case Reason::OpenIntent:
    return "an append intent with no outcome";
  case Reason::UncertainAbort:
    return "the append may have run before it was abandoned";
  case Reason::AmbiguousJournal:
    return "records out of order";
  case Reason::SpanBeyondPermascroll:
    return "its span ends past the permascroll";
  case Reason::PublicationIncomplete:
    return "its publication is incomplete";
  case Reason::InvalidTranslation:
    return "its intervals disagree";
  case Reason::MapConflict:
    return "its translation conflicts with another";
  }
  return "unknown";
}

void printHelpREPL() {
  std::cout
      << "VQL Interactive REPL Commands:\n"
      << "  <query>               Execute VQL query expression\n"
      << "  :view [dims]          Toggle visible connection view or set "
         "viewing dimensions\n"
      << "  :dims [dims]          Inspect or set active viewing "
         "dimensions (e.g. d.1,d.2,d.3)\n"
      << "  :ascii                Toggle ASCII art output mode\n"
      << "  :engine vortex        Select the Vortex VM execution engine\n"
      << "  :store <label> <path> Load additional xanadoc/slice store\n"
      << "  :open [-w|-wc|-a|-ac] <name> <path>  Open a file read-only (or "
         "for write -w / append -a, c to create); bind its capability to "
         "$name for `effect { ... }` script I/O; writing needs "
         "--io-write-root\n"
      << "  :timeout [MS|off]     Show or set how long a parked script may "
         "wait before it is abandoned\n"
      << "  :captures             List captures: the store, what recovery "
         "found, orphan bytes and pinned captures\n"
      << "  :reconcile ID release|promote [START LEN]  Decide one pinned "
         "capture: release it, or promote it to the span its journal names "
         "(or START LEN of the permascroll)\n"
      << "  :forget               Drop what this session read but did not "
         "capture and unbind every variable that is not an I/O handle, so "
         ":save works again\n"
      << "  :stores               List currently loaded stores\n"
      << "  :save <path>          Save the last query rows as a result slice\n"
      << zigzag::LensRepl::helpText
      << "  :ast <query>          Print AST dump for query\n"
      << "  :asm <query>          Print compiled bytecode for query\n"
      << "  :evidence [command]   Private evidence case (use :evidence help)\n"
      << "  :evidence mail-credential \"NAME\" LIFETIME_MS  Bind IMAP "
         "credentials privately\n"
      << "  :evidence mail-submit-credential \"INTENT_ID\" LIFETIME_MS  Bind "
         "SMTP credentials privately\n"
      << "  :evidence irc-credential \"NAME\" LIFETIME_MS  Bind IRC SASL "
         "credentials privately\n"
      << "  :help                 Show this help message\n"
      << "  :quit / :exit         Exit REPL\n";
}

std::vector<std::string> parseDimensionNames(std::string_view input) {
  std::vector<std::string> dims;
  std::string current;
  for (char ch : input) {
    if (ch == ',' || ch == '/' || ch == ';' || ch == ' ') {
      if (!current.empty()) {
        if (!current.starts_with("d.") &&
            (current == "1" || current == "2" || current == "3")) {
          dims.push_back("d." + current);
        } else {
          dims.push_back(current);
        }
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    if (!current.starts_with("d.") &&
        (current == "1" || current == "2" || current == "3")) {
      dims.push_back("d." + current);
    } else {
      dims.push_back(current);
    }
  }
  if (dims.empty()) {
    return {"d.1", "d.2", "d.3"};
  }
  return dims;
}

std::vector<std::string> reorderArgs(int argc, char *argv[]) {
  std::vector<std::string> options;
  std::vector<std::string> positionals;
  options.emplace_back(argv[0]);

  const std::vector<std::string> valueOptions = {"-e",
                                                 "--eval",
                                                 "-f",
                                                 "--file",
                                                 "-o",
                                                 "--output-store",
                                                 "--engine",
                                                 "--format",
                                                 "--permascroll",
                                                 "--io-file",
                                                 "--io-write-root",
                                                 "--io-write-bytes",
                                                 "--capture-dir",
                                                 "-d",
                                                 "--dims",
                                                 "--view-dims",
                                                 "--script-timeout"};

  bool pastDoubleDash = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (pastDoubleDash) {
      positionals.push_back(arg);
      continue;
    }
    if (arg == "--") {
      pastDoubleDash = true;
      continue;
    }
    bool isValueOpt = false;
    for (const auto &opt : valueOptions) {
      if (arg == opt) {
        isValueOpt = true;
        break;
      }
    }
    if (isValueOpt) {
      if (i + 1 < argc) {
        std::string next = argv[++i];
        if (next == "-" && (arg == "-o" || arg == "--output-store")) {
          options.push_back("--output-store=-");
        } else {
          options.push_back(arg);
          options.push_back(std::move(next));
        }
      } else {
        options.push_back(arg);
      }
    } else if (arg.starts_with("-") && arg != "-") {
      options.push_back(arg);
    } else {
      positionals.push_back(arg);
    }
  }

  options.insert(options.end(), positionals.begin(), positionals.end());
  return options;
}

} // namespace

// Catches std::exception and reports it; anything else (a real bug, not a
// user-facing failure) is deliberately left to terminate with a backtrace
// rather than be swallowed into a generic error message.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[]) {
  if (const auto mesh = zigzag::vortex::mesh::runMeshCommand(
          zigzag::vortex::mesh::Frontend::Vquery, argc, argv))
    return *mesh;
  argparse::ArgumentParser program("vquery", "1.0.0");

  program.add_argument("-e", "--eval")
      .help("Execute inline query string")
      .default_value(std::string(""));

  program.add_argument("-f", "--file")
      .help("Path to VQL query file to execute")
      .default_value(std::string(""));

  program.add_argument("-o", "--output-store")
      .help(
          "Path to result store (created if non-existent, updated if existing)")
      .default_value(std::string(""));

  program.add_argument("--lens")
      .help("Open a lens URI (path, dir:, git:, sql:, cfg:, log:, journal:, "
            "union:, meta:<label>) and print its projected tree")
      .default_value(std::string{});
  program.add_argument("--in-place")
      .help("Modify the primary input store in-place")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--engine")
      .help("Execution engine: 'vortex' (compile to bytecode and run in VM)")
      .default_value(std::string("vortex"));

  program.add_argument("--format")
      .help("Output format: 'text', 'ascii', 'view'/'grid', 'cells', 'table'")
      .default_value(std::string("text"));

  program.add_argument("--ascii")
      .help("Shorthand for --format ascii (render ASCII art topology)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--view")
      .help(
          "Shorthand for --format view (visible cell connections along viewing "
          "dimensions)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--grid")
      .help("Alias for --view")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("-d", "--dims", "--view-dims")
      .help("Active viewing dimensions for cell connection view "
            "(comma-separated, default: d.1,d.2,d.3)")
      .default_value(std::string("d.1,d.2,d.3"));

  program.add_argument("--text")
      .help("Shorthand for --format text (default)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--cells")
      .help("Shorthand for --format cells (render cell IDs)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--permascroll")
      .help("Path to permascroll storage directory")
      .default_value(std::string(""));

  program.add_argument("--io-file")
      .help("Open a file read-only for script I/O actions and bind its "
            "capability to $NAME (NAME=PATH); may be repeated. The open runs "
            "asynchronously; read it with an `effect { ... }` script action")
      .append()
      .default_value(std::vector<std::string>{});

  program.add_argument("--io-write-root")
      .help("Allow script I/O actions to open files for write or append "
            "beneath DIR (symlinks and '..' are resolved first); may be "
            "repeated. With none given no script can change any file")
      .append()
      .default_value(std::vector<std::string>{});

  program.add_argument("--io-write-bytes")
      .help("The most bytes one script write or append may carry (default "
            "1048576)")
      .default_value(std::string{});

  program.add_argument("--capture-dir")
      .help("Where `capture` keeps its journal and its receipt store (default: "
            "beside the permascroll, under 'capture' instead of "
            "'permascroll'). Built when a script first captures")
      .default_value(std::string{});

  program.add_argument("--script-timeout")
      .help("How long a script waiting on I/O may stay parked before it is "
            "abandoned with VQL_SCRIPT_TIMEOUT: a number of milliseconds, or "
            "'off' (default 30000)")
      .default_value(std::string("30000"));

  program.add_argument("--dump-ast")
      .help("Print formatted AST before execution")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--dump-asm")
      .help("Print compiled Vortex bytecode (available with --engine vortex)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--headless")
      .help("Run headless (no display)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("stores")
      .help("Paths to input xanadoc/slice stores (primary store first)")
      .remaining();

  try {
    auto args = reorderArgs(argc, argv);
    program.parse_args(args);
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    std::cerr << program;
    return 1;
  }

  // Determine output format
  auto format = program.get<std::string>("--format");
  if (program.get<bool>("--ascii")) {
    format = "ascii";
  } else if (program.get<bool>("--view") || program.get<bool>("--grid")) {
    format = "view";
  } else if (program.get<bool>("--text")) {
    format = "text";
  } else if (program.get<bool>("--cells")) {
    format = "cells";
  }

  auto dimsOpt                          = program.get<std::string>("--dims");
  std::vector<std::string> viewDimNames = parseDimensionNames(dimsOpt);

  auto engineMode = program.get<std::string>("--engine");
  if (engineMode != "vortex") {
    std::cerr << "Unsupported execution engine '" << engineMode
              << "'; VQL runs through the Vortex VM\n";
    return 2;
  }
  bool dumpAst = program.get<bool>("--dump-ast");
  bool dumpAsm = program.get<bool>("--dump-asm");

  std::vector<std::string> storePaths;
  if (program.present<std::vector<std::string>>("stores")) {
    storePaths = program.get<std::vector<std::string>>("stores");
  }
  std::unique_ptr<xanadu::TemporaryStreamStore> stdinStore;
  for (auto &path : storePaths) {
    if (path == "-") {
      if (stdinStore) {
        std::cerr << "Only one input store may use standard input.\n";
        return 1;
      }
      stdinStore = std::make_unique<xanadu::TemporaryStreamStore>(std::cin);
      path       = stdinStore->path().string();
    }
  }

  // Setup permascroll and MultiStoreCoordinator
  auto permaDir = program.get<std::string>("--permascroll");
  xanadu::UserPermascroll::Config permaConfig;
  if (!permaDir.empty()) {
    // A named permascroll that is not there is a mistyped path: reading
    // against it answers every query blank, and opening it would create it.
    if (!std::filesystem::exists(permaDir)) {
      std::cerr << "Error: no permascroll at " << permaDir << "\n";
      return 1;
    }
    permaConfig.storageDir = permaDir;
  }

  const auto permaSource = permaConfig.storageDir;
  // Native stores address the registered author scroll; nearby directories
  // can be registry containers or belong to another author.
  auto permascroll =
      permaConfig.storageDir.empty()
          ? xanadu::PermascrollRegistry::instance().defaultUser()
          : std::make_shared<xanadu::UserPermascroll>(std::move(permaConfig));
  // A store holds no text of its own; read against a permascroll that is
  // not the one it was written against, every answer comes back blank. Say
  // so rather than print nothing.
  if (!storePaths.empty() && 0 == permascroll->size()) {
    std::cerr << "Warning: the permascroll"
              << (permaSource.empty() ? std::string{}
                                      : " at " + permaSource.string())
              << " holds no text, so results will have none; pass "
                 "--permascroll with the one the store was written against\n";
  }
  xanadu::MultiStoreCoordinator coordinator;

  std::string primaryPath;
  if (!storePaths.empty()) {
    primaryPath = storePaths[0];
    for (std::size_t i = 0; i < storePaths.size(); ++i) {
      const std::string &path = storePaths[i];
      std::string label       = extractStoreLabel(path);
      std::string role        = (i == 0) ? "primary" : "library";

      if (!std::filesystem::exists(path)) {
        // Querying a store that is not there would answer from nothing and
        // look like an empty result.
        std::cerr << "Error: store path does not exist: " << path << "\n";
        return 1;
      }
      coordinator.loadAndAddStore(label, role, path, permascroll);
    }
  }

  // meta:<label> binds the coordinator's matching store -- or its
  // primary when the label is empty -- as a frozen fold for MetaLens.
  const zigzag::LensRepl::MetaSource lensMeta =
      [&coordinator](std::string_view label)
      -> std::expected<std::pair<zigzag::Manifold, const xanadu::SpanReader *>,
                       std::string> {
    const auto info = label.empty() ? coordinator.primaryStore()
                                    : coordinator.findStore(label);
    if (!info || info->store == nullptr) {
      return std::unexpected("no store named '" + std::string{label} + "'");
    }
    const auto &store = info->store;
    return std::pair{store->rebuildManifold(store->primaryCurrentVersion()),
                     store.get()};
  };
  // The coordinator's core, not an internal fold: lens cells land in the
  // same arena VQL queries, so ':lens' projects directly into the query
  // space.
  zigzag::LensRepl lensRepl(coordinator.core());

  // --lens is the programmatic surface: print the projection and exit.
  {
    const auto lensUri = program.get<std::string>("--lens");
    if (!lensUri.empty()) {
      if (lensUri.starts_with("meta:")) {
        std::ostringstream buf;
        lensRepl.run(":lens " + lensUri, buf, lensMeta);
        std::cout << buf.str();
        return buf.str().contains(" cells,") ? 0 : 1;
      }
      return zigzag::runLensRaster(lensUri, std::cout);
    }
  }

  // Obtain query text
  auto queryText = program.get<std::string>("--eval");
  auto queryFile = program.get<std::string>("--file");

  if (queryText.empty() && !queryFile.empty()) {
    std::ifstream ifs(queryFile);
    if (!ifs) {
      std::cerr << "Error: cannot open query file: " << queryFile << "\n";
      return 1;
    }
    queryText.assign((std::istreambuf_iterator<char>(ifs)),
                     (std::istreambuf_iterator<char>()));
  }

  auto scriptTimeout =
      std::optional<std::chrono::milliseconds>{defaultScriptTimeout};
  if (const auto parsed =
          parseScriptTimeout(program.get<std::string>("--script-timeout"))) {
    scriptTimeout = *parsed;
  } else {
    std::cerr << "Error: --script-timeout expects a positive number of "
                 "milliseconds or 'off'\n";
    return 1;
  }
  // What the last query found: what -o and :save write.
  std::vector<zigzag::CellRef> lastResults;
  xanadu::vql::VQLEngine engine(coordinator);
  // One VM for the process: the core has a single write guard, and the I/O
  // host parks script actions on this VM's cursors.
  zigzag::vortex::VortexVM vm(coordinator.core());
  // What a capture writes through, declared before the host that runs workers
  // over them: the host's destructor joins those workers, and they must find
  // the sink and the publisher alive. Built on the first `capture`.
  std::unique_ptr<xanadu::vortex::UserPermascrollCaptureSink> captureSink;
  std::unique_ptr<xanadu::Store> captureStore;
  std::unique_ptr<xanadu::vortex::CapturePublisher> capturePublisher;
  // Script I/O is built on first use, so a plain query never starts workers.
  std::unique_ptr<xanadu::vortex::FileReadHost> io;
  // Declared after `io`, so the runner dies first and cancels its waiters on a
  // live host.
  std::unique_ptr<xanadu::vql::ScriptRunner> ioRunner;
  xanadu::vortex::FileReadHostOptions ioOptions;
  {
    // Authority to write is the invoking human's: it comes from these flags
    // and from nowhere a script can reach.
    std::vector<std::filesystem::path> writeRoots;
    for (const auto &root :
         program.get<std::vector<std::string>>("--io-write-root"))
      writeRoots.emplace_back(root);
    if (!writeRoots.empty())
      ioOptions.writeGrant = xanadu::vortex::writeRootGrant(writeRoots);
    if (const auto bound = program.get<std::string>("--io-write-bytes");
        !bound.empty()) {
      std::size_t count{};
      const auto *const end = bound.data() + bound.size();
      const auto parsed     = std::from_chars(bound.data(), end, count);
      if (parsed.ec != std::errc{} || parsed.ptr != end || count == 0) {
        std::cerr << "Error: --io-write-bytes expects a positive number of "
                     "bytes\n";
        return 1;
      }
      ioOptions.writeBytes = count;
    }
  }
  const auto ioHost = [&]() -> xanadu::vortex::FileReadHost & {
    if (!io)
      io = std::make_unique<xanadu::vortex::FileReadHost>(
          vm, coordinator.arena(), ioOptions);
    return *io;
  };
  // The capture store lives in a dedicated directory beside the permascroll
  // (design §5.6): the publisher lands on the store's latest and needs one
  // writer, which a document store another program opens cannot promise.
  const auto captureDirectory = [&]() -> std::filesystem::path {
    if (const auto named = program.get<std::string>("--capture-dir");
        !named.empty())
      return named;
    const auto scroll = permascroll->config().storageDir;
    if (scroll.parent_path().filename() == "permascroll")
      return scroll.parent_path().parent_path() / "capture" / scroll.filename();
    return scroll.parent_path() / (scroll.filename().string() + ".capture");
  };
  const auto ensureCapture =
      [&]() -> std::expected<xanadu::vortex::CaptureHost *, std::string> {
    auto &host = ioHost();
    if (auto *built = host.capture()) return built;
    const auto directory = captureDirectory();
    try {
      // Each directory this creates is an entry in its parent that must be
      // durable before the journal's first intent: a power loss after the
      // append would otherwise leave bytes with no intent to find them.
      std::vector<std::filesystem::path> created;
      for (auto at = directory; !at.empty() && !std::filesystem::exists(at);
           at      = at.parent_path())
        created.push_back(at);
      std::filesystem::create_directories(directory);
      for (const auto &made : created) {
        if (!xanadu::vortex::syncParentDirectory(made))
          GLEDITOR_LOG_WARN("vquery.capture",
                            "the entry of {} could not be made durable",
                            made.string());
      }
      if (std::filesystem::exists(directory / "ops.nodes")) {
        captureStore = xanadu::loadStore(directory, permascroll);
      } else {
        captureStore = std::make_unique<xanadu::Store>(permascroll);
      }
      captureSink =
          std::make_unique<xanadu::vortex::UserPermascrollCaptureSink>(
              *permascroll);
      capturePublisher = std::make_unique<xanadu::vortex::CapturePublisher>(
          *captureStore, directory);
      return &host.attachCapture(
          *captureSink, capturePublisher.get(),
          xanadu::vortex::CaptureHostOptions{.journalPath = directory /
                                                            "capture.journal"});
    } catch (const std::exception &error) {
      return std::unexpected("the capture store at " + directory.string() +
                             " could not be opened: " + error.what());
    }
  };
  // Open @p path through the host and bind its capability to $name. The open
  // is admitted and then polled through the pump; nothing here reads a file.
  const auto bindFile = [&](const std::string &name, const std::string &path,
                            const std::uint64_t rights =
                                xanadu::vortex::fileRightRead) -> bool {
    auto &host      = ioHost();
    const auto open = host.beginOpen(path, rights);
    if (!open) {
      std::cerr << "Open failed: " << open.error().code << ": "
                << open.error().message << "\n";
      return false;
    }
    while (true) {
      if (const auto opened = host.openedCapability(*open)) {
        if (!*opened) {
          std::cerr << "Open failed: " << path << ": "
                    << opened->error().message << "\n";
          return false;
        }
        engine.setVariable(name, (*opened)->cell);
        return true;
      }
      host.pump(ioPumpBudget);
      std::this_thread::sleep_for(ioIdleSleep);
    }
  };
  for (const auto &spec : program.get<std::vector<std::string>>("--io-file")) {
    const auto split = spec.find('=');
    if (split == std::string::npos || split == 0 || split + 1 == spec.size()) {
      std::cerr << "Error: --io-file expects NAME=PATH, got '" << spec << "'\n";
      return 1;
    }
    if (!bindFile(spec.substr(0, split), spec.substr(split + 1))) return 1;
  }
  // Run a script holding I/O actions: the runner parks at an unfinished
  // await and the driver pumps the host until it wakes the same action.
  const auto runIoScript = [&](xanadu::vql::QueryExpression ast)
      -> std::expected<std::vector<zigzag::CellRef>,
                       xanadu::vortex::FrontendDiagnostic> {
    auto &host = ioHost();
    // One runner for the session: it retires an action's request when the
    // next action replaces it, so the rows printed from this script stay
    // readable until then and a long session holds one request, not all.
    if (!ioRunner) {
      auto ports              = xanadu::vql::fileScriptPorts(host);
      ports.ensureCaptureHost = ensureCapture;
      ioRunner = std::make_unique<xanadu::vql::ScriptRunner>(engine, vm,
                                                             std::move(ports));
    }
    auto &runner = *ioRunner;
    runner.setScriptTimeout(scriptTimeout);
    return xanadu::vql::driveToCompletion(
        runner, runner.run(std::move(ast)), [&] { host.pump(ioPumpBudget); },
        [] { std::this_thread::sleep_for(ioIdleSleep); });
  };
  // Query Execution Helper
  bool hasResults = false;
  // Input is transient unless the author captured it (design §4.7, owner
  // decision Q2). The rule is coverage over the session, not taint: every
  // payload any script turned into text since the session began is an
  // exposure, the text outlives its script as a session variable, and so the
  // runner says whether committed captures hold every one whatever line made
  // the rows. A row that is itself a runtime handle names native state no
  // store can hold and is refused on every line, including a bare `return
  // $src` over a capability that `--io-file` or `:open` bound.
  const auto refuseDurableAction = [&]() -> bool {
    // Handles first: promotion would refuse them deep inside, so they are
    // refused here by name, with the road to what the author meant.
    const auto &arena = coordinator.arena();
    for (const auto cell : lastResults) {
      const auto incarnation = arena.incarnationOf(cell);
      if (!incarnation) continue;
      const auto tag = xanadu::vortex::ioCellTag(arena, cell, *incarnation);
      if (!tag || *tag == xanadu::vortex::IoCellTag::Outcome) continue;
      std::cerr << xanadu::vortex::frontend_codes::vqlPersistHandle
                << ": a result row is a runtime I/O handle ("
                << xanadu::vortex::toString(*tag)
                << "), which no store can hold; return "
                   "io_captured($c) to persist what a capture made "
                   "durable\n";
      return true;
    }
    const auto verdict = ioRunner ? ioRunner->persistability() : std::nullopt;
    if (!verdict || verdict->persistable()) return false;
    std::cerr << xanadu::vortex::frontend_codes::vqlPersistNeedsCapture
              << ": this session turned " << verdict->uncovered << " payload"
              << (1 == verdict->uncovered ? "" : "s")
              << " it read into text without capturing "
                 "all of it; input is transient unless explicitly captured "
                 "(effect { capture($r) })\n";
    return true;
  };
  auto runQuery = [&](std::string_view qStr) -> bool {
    // Parse AST
    xanadu::vql::QueryExpression ast;
    try {
      xanadu::vql::Parser parser(qStr);
      ast = parser.parseQuery();
    } catch (const xanadu::vql::ParseError &err) {
      std::cerr << "Parse error: " << err.what() << "\n";
      return false;
    }

    if (dumpAst) {
      std::cout << xanadu::vql::AsciiVisualizer::renderAST(ast) << "\n";
    }

    if (const auto root = lensRepl.root()) engine.setVariable("lens", *root);
    const bool ranAction = xanadu::vql::hasIoEffect(ast);
    std::vector<zigzag::CellRef> results;
    if (ranAction) {
      // The first action this script admits retires the rows the last one
      // returned, so nothing from before may be named afterwards, whether
      // the script ends or fails: -o, --in-place and :save of a failed
      // script find no results rather than cells that no longer exist.
      lastResults.clear();
      hasResults = false;
      auto rows  = runIoScript(std::move(ast));
      if (!rows) {
        std::cerr << "Query failed: " << rows.error().code << ": "
                  << rows.error().message << "\n";
        return false;
      }
      results = *std::move(rows);
    } else {
      xanadu::vql::QueryExecutionOptions executionOptions;
      executionOptions.mode = xanadu::vql::QueryExecutionMode::Vortex;
      xanadu::vql::QueryExecutor executor(engine, vm, executionOptions);
      const auto execution = executor.execute(ast);
      if (dumpAsm &&
          executionOptions.mode == xanadu::vql::QueryExecutionMode::Vortex)
        std::cout << executor.disassembly() << "\n";
      if (!execution) {
        // A stale name answers a typed code; the operator that raised it is
        // the engine's, not something the author can act on.
        const auto &message = execution.error().message;
        const auto typed =
            message.find(xanadu::vortex::frontend_codes::vqlStaleBinding);
        std::cerr << "Query failed: "
                  << (typed == std::string::npos ? message
                                                 : message.substr(typed))
                  << "\n";
        return false;
      }
      results = *execution;
    }
    // A name this line made is recorded while any handle it copied is live.
    if (ioRunner) ioRunner->noteBindings();

    lastResults = results;
    hasResults  = true;

    // Format results
    if (format == "ascii") {
      std::cout << xanadu::vql::AsciiVisualizer::renderQueryResults(
                       coordinator.arena(), results)
                << "\n";
    } else if (format == "view" || format == "grid") {
      std::vector<xanadu::vql::ViewDimension> vdims;
      vdims.reserve(viewDimNames.size());
      for (const auto &dname : viewDimNames) {
        vdims.push_back(
            {.name = dname, .dim = coordinator.resolveDimension(dname)});
      }
      std::cout << xanadu::vql::AsciiVisualizer::renderCellConnections(
                       coordinator.arena(), results, vdims)
                << "\n";
    } else if (format == "cells") {
      std::cout << "[";
      for (std::size_t i = 0; i < results.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << "#" << results[i];
      }
      std::cout << "]\n";
    } else {
      // Text output
      for (zigzag::CellRef c : results) {
        const auto val = coordinator.core().render(c);
        if (!val) continue;
        if (std::holds_alternative<std::string>(*val)) {
          auto text = std::get<std::string>(*val);
          // A captured cell's content is a span of the author's permascroll,
          // which the arena alone cannot read: without this the REPL printed
          // an empty line for the bytes the author had just captured.
          if (text.empty()) {
            const auto content = coordinator.arena().contentOf(c);
            if (std::ranges::any_of(
                    content, [](const xanadu::PrimediaSpan &span) {
                      return xanadu::scratchScroll != span.scroll;
                    })) {
              text = coordinator.arena().textOf(c, *permascroll);
            }
          }
          std::cout << text << "\n";
        } else if (std::holds_alternative<double>(*val)) {
          std::cout << std::get<double>(*val) << "\n";
        } else if (std::holds_alternative<std::int64_t>(*val)) {
          std::cout << std::get<std::int64_t>(*val) << "\n";
        } else if (std::holds_alternative<bool>(*val)) {
          std::cout << (std::get<bool>(*val) ? "true" : "false") << "\n";
        }
      }
    }

    return true;
  };

  // If query is provided, execute batch and handle output persistence
  if (!queryText.empty()) {
    const auto outPath      = program.get<std::string>("--output-store");
    const bool streamOutput = outPath == "-";
    auto *originalOutput    = std::cout.rdbuf();
    if (streamOutput) std::cout.rdbuf(std::cerr.rdbuf());
    bool ok = runQuery(queryText);
    if (!ok) return 1;
    if ((program.get<bool>("--in-place") || !outPath.empty()) &&
        refuseDurableAction()) {
      std::cout.rdbuf(originalOutput);
      return 1;
    }

    // In place: what the query minted -- cells that exist only in the arena
    // -- is promoted into the primary store with everything reachable from
    // it. A query that only read has nothing to write, and says so rather
    // than claim a save that changed nothing.
    if (program.get<bool>("--in-place") && !primaryPath.empty()) {
      const auto primStore = coordinator.primaryStore();
      if (!primStore || !primStore->store) {
        std::cerr << "Error: no primary store to write into\n";
        return 1;
      }
      auto &store         = *primStore->store;
      auto &arena         = coordinator.arena();
      std::size_t written = 0;
      for (const auto result : lastResults) {
        if (!zigzag::isEphemeral(result) || arena.isProxy(result) ||
            arena.resolveForeign(result)) {
          continue;
        }
        // A captured cell is linked into its capture's receipt, and the
        // receipt into the runtime I/O graph that promotion refuses. What the
        // author wants written is the span, which already has an address in
        // the permascroll, so a detached cell over that one span is promoted
        // instead. Captured cells are the ones on the receipt's range rank.
        auto promotable    = result;
        const auto content = arena.contentOf(result);
        if (io && 1 == content.size() &&
            xanadu::scratchScroll != content[0].scroll &&
            arena.linked(
                result,
                io->bridge().schema().dim(xanadu::vortex::IoDimension::Range),
                zigzag::DimVector::NEG)) {
          promotable = arena.makeCell(content[0]);
        }
        const auto promoted =
            zigzag::promoteOrRefuse(store, store.latest(), arena, promotable);
        if (promoted) {
          written += promoted->cells.size();
        } else {
          // Said, not swallowed: a result the store cannot name (runtime-only
          // state, bytes it cannot address) is a refusal the author should see.
          std::cerr << "Not written in place: "
                    << zigzag::toString(promoted.error()) << "\n";
        }
      }
      if (0 == written) {
        std::cout << "Nothing to write in place: the query minted no cells\n";
      } else {
        store.save(primaryPath);
        std::cout << "Wrote " << written << " cell" << (1 == written ? "" : "s")
                  << " into " << primaryPath << "\n";
      }
    }

    // Handle output store
    if (!outPath.empty()) {
      if (!streamOutput && std::filesystem::exists(outPath)) {
        std::cerr << "Result path already exists: " << outPath << "\n";
        return 1;
      }
      std::unique_ptr<xanadu::TemporaryStreamStore> streamStore;
      if (streamOutput)
        streamStore = std::make_unique<xanadu::TemporaryStreamStore>();
      const auto destination =
          streamOutput ? streamStore->path() : std::filesystem::path(outPath);
      xanadu::Store outStore(permascroll);
      xanadu::writeResultSlice(
          outStore, resultRows(coordinator, lastResults, captureStore.get(),
                               stdinStore ? stdinStore->path().string() : ""));
      outStore.save(destination.string());
      if (streamOutput) {
        std::cout.flush();
        std::cout.rdbuf(originalOutput);
        xanadu::writeStoreStream(destination, std::cout);
        std::cout.flush();
      } else {
        std::cout << "Result store written to: " << outPath << "\n";
      }
    }

    return 0;
  }

  // Interactive REPL Mode
  xanadu::evidence::EvidenceController evidence(permascroll);
  std::cout << "VQL Interactive REPL (vquery v1.0.0)\n"
            << "Type queries or :help for commands. Press Ctrl+D to exit.\n";

  std::string line;
  while (true) {
    std::cout << "vql> ";
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }

    if (line.empty()) continue;

    if (line == ":quit" || line == ":exit") {
      break;
    }
    if (line == ":help") {
      printHelpREPL();
    } else if (vquery::isMailCredentialCommand(line)) {
      vquery::handleMailCredentialCommand(evidence, line, std::cout, std::cerr);
    } else if (line == ":evidence" || line.starts_with(":evidence ")) {
      const bool success = evidence.execute(line);
      const auto state   = evidence.snapshot();
      std::cout << (success ? "" : "Evidence error: ") << state.status << "\n";
      for (const auto &detail : state.details) std::cout << detail << "\n";
    } else if (line == ":ascii") {
      format = (format == "ascii") ? "text" : "ascii";
      std::cout << "Output mode set to: " << format << "\n";
    } else if (line == ":view") {
      format = (format == "view") ? "text" : "view";
      std::cout << "Output mode set to: " << format << " (dimensions: ";
      for (std::size_t i = 0; i < viewDimNames.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << viewDimNames[i];
      }
      std::cout << ")\n";
    } else if (line.starts_with(":view ")) {
      std::string arg = line.substr(6);
      viewDimNames    = parseDimensionNames(arg);
      format          = "view";
      std::cout << "Output mode set to view with dimensions: ";
      for (std::size_t i = 0; i < viewDimNames.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << viewDimNames[i];
      }
      std::cout << "\n";
    } else if (line == ":dims") {
      std::cout << "Active viewing dimensions: ";
      for (std::size_t i = 0; i < viewDimNames.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << viewDimNames[i];
      }
      std::cout << "\n";
    } else if (line.starts_with(":dims ")) {
      std::string arg = line.substr(6);
      viewDimNames    = parseDimensionNames(arg);
      std::cout << "Viewing dimensions set to: ";
      for (std::size_t i = 0; i < viewDimNames.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << viewDimNames[i];
      }
      std::cout << "\n";
    } else if (line.starts_with(":engine")) {
      std::istringstream iss(line);
      std::string cmd, eng;
      iss >> cmd >> eng;
      if (eng == "vortex") {
        engineMode = eng;
        std::cout << "Engine set to: " << engineMode << "\n";
      } else if (eng == "direct") {
        std::cout << "Unsupported execution engine 'direct'; VQL runs through "
                     "the Vortex VM\n";
      } else {
        std::cout << "Unknown engine. Use :engine vortex\n";
      }
    } else if (zigzag::LensRepl::handles(line)) {
      lensRepl.run(line, std::cout, lensMeta);
    } else if (line == ":timeout" || line.starts_with(":timeout ")) {
      const auto argument =
          line.size() > 8
              ? line.substr(line.find_first_not_of(' ', 8) == std::string::npos
                                ? line.size()
                                : line.find_first_not_of(' ', 8))
              : std::string{};
      if (argument.empty()) {
        if (scriptTimeout)
          std::cout << "Script timeout: " << scriptTimeout->count() << " ms\n";
        else
          std::cout << "Script timeout: off\n";
      } else if (const auto parsed = parseScriptTimeout(argument)) {
        scriptTimeout = *parsed;
        std::cout << "Script timeout set\n";
      } else {
        std::cout << "Usage: :timeout [MS|off]\n";
      }
    } else if (line.starts_with(":open ")) {
      std::istringstream iss(line);
      std::string cmd, name;
      iss >> cmd >> name;
      // An optional mode flag leads: -w (write), -a (append), each with a
      // trailing c to create the file (and fail if it exists). The host's
      // write grant still decides; a flag asks, it does not authorise.
      std::uint64_t rights = xanadu::vortex::fileRightRead;
      bool flagged         = false;
      if (name == "-w" || name == "-wc" || name == "-a" || name == "-ac") {
        flagged = true;
        rights  = name[1] == 'w' ? xanadu::vortex::fileRightWrite
                                 : xanadu::vortex::fileRightAppend;
        if (name.size() == 3) rights |= xanadu::vortex::fileRightCreate;
        iss >> name;
      }
      std::string path;
      // The path is the rest of the line, so one with spaces is not cut.
      std::getline(iss >> std::ws, path);
      if (name.empty() || path.empty()) {
        std::cout << "Usage: :open [-w|-wc|-a|-ac] <name> <path>\n";
      } else if (bindFile(name, path, rights)) {
        std::cout << "Opened " << path << (flagged ? " for change" : "")
                  << " as $" << name << "\n";
      }
    } else if (line == ":captures") {
      // Reading the capture store is not a capture: with no stack built and no
      // journal on disk there is nothing to report and nothing is created.
      // With a journal, the stack is built (as a first `capture` would) and
      // its recovery run -- the owner-thread exception design §8 names -- so
      // what a crashed session left is what this prints.
      const auto journal = captureDirectory() / "capture.journal";
      if (!io || io->capture() == nullptr) {
        std::error_code missing;
        if (!std::filesystem::exists(journal, missing)) {
          std::cout << "No captures: nothing has been captured (no journal at "
                    << journal.string() << ")\n";
          continue;
        }
      }
      const auto stack = ensureCapture();
      if (!stack) {
        std::cerr << "Captures unavailable: " << stack.error() << "\n";
        continue;
      }
      auto &captures = **stack;
      if (const auto ready = captures.ready(); !ready) {
        std::cerr << "Capture recovery refused: "
                  << xanadu::vortex::toString(ready.error().code) << " detail "
                  << ready.error().stableDetail << "\n";
        continue;
      }
      const auto &coordinator = captures.coordinator();
      const auto &report      = captures.recoveredReport();
      const auto counts       = coordinator.stateCounts();
      const auto pins         = coordinator.pinnedDetails();
      std::cout << "Capture store: " << captureDirectory().string() << "\n"
                << "Recovered captures: "
                << (report ? report->entries.size() : 0U) << "\n"
                << "Orphan bytes: " << (report ? report->orphanBytes : 0U)
                << "\n"
                << "Translations: " << counts.translationSlots << " (evicted "
                << counts.evictedTranslations << ")\n"
                << "Pinned captures: " << pins.size() << "\n";
      for (const auto &pin : pins) {
        std::cout << "  capture " << pin.capture << ": "
                  << captureReasonName(pin.reason) << ", " << pin.appendLength
                  << " bytes may be appended";
        if (pin.span)
          std::cout << ", journal names span " << pin.span->start << "+"
                    << pin.span->length;
        std::cout << "\n";
      }
    } else if (line == ":reconcile" || line.starts_with(":reconcile ")) {
      const auto parsed = parseReconcile(line);
      const auto id     = parsed ? parsed->first : xanadu::vortex::CaptureId{};
      std::optional<xanadu::vortex::CaptureReconcileDecision> decision;
      if (parsed) decision = parsed->second;
      const auto journal = captureDirectory() / "capture.journal";
      std::error_code missing;
      if (!decision) {
        std::cout << "Usage: :reconcile ID release | :reconcile ID promote "
                     "[START LEN]\n";
      } else if ((!io || io->capture() == nullptr) &&
                 !std::filesystem::exists(journal, missing)) {
        std::cout << "Nothing to reconcile: no capture journal at "
                  << journal.string() << "\n";
      } else if (const auto stack = ensureCapture(); !stack) {
        std::cerr << "Captures unavailable: " << stack.error() << "\n";
      } else if (const auto ready = (*stack)->ready(); !ready) {
        std::cerr << "Capture recovery refused: "
                  << xanadu::vortex::toString(ready.error().code) << " detail "
                  << ready.error().stableDetail << "\n";
      } else {
        // One decision, once: a refusal is printed and the pin kept, never
        // retried here.
        auto &coordinator = (*stack)->coordinator();
        const auto done   = coordinator.reconcile(id, *decision);
        if (!done) {
          std::cerr << "Reconcile refused: "
                    << xanadu::vortex::toString(done.error().code) << " detail "
                    << done.error().stableDetail << " (capture " << id
                    << " stays pinned)\n";
        } else {
          std::cout << (done->resolution ==
                                xanadu::vortex::CaptureReconcileResult::
                                    Resolution::Promoted
                            ? "Promoted capture "
                            : "Released capture ")
                    << id << "\n";
          if (done->publicationPending) {
            const auto resumed = coordinator.resumePublications();
            if (resumed)
              std::cout << "Published " << *resumed << " capture(s)\n";
            else
              std::cerr << "Publication still pending: "
                        << xanadu::vortex::toString(resumed.error().code)
                        << " detail " << resumed.error().stableDetail << "\n";
          }
        }
      }
    } else if (line == ":forget") {
      // The owner's way out of a refused :save: coverage is what the persist
      // rule tracks, not taint, so the text a variable carries cannot be traced
      // to its read. Every variable that is not a runtime handle goes.
      const auto report =
          ioRunner ? ioRunner->forget() : xanadu::vql::ForgetReport{};
      if (report.exposures == 0 && report.unbound.empty()) {
        std::cout << "Nothing to forget: every payload this session read is "
                     "captured or none was read\n";
      } else {
        std::cout << "Forgot " << report.exposures << " uncaptured payload"
                  << (1 == report.exposures ? "" : "s") << "; unbound";
        auto unbound = report.unbound;
        std::ranges::sort(unbound);
        for (const auto &name : unbound) std::cout << " $" << name;
        if (report.unbound.empty()) std::cout << " nothing";
        std::cout << "\n";
        // Rows may be built from the text just dropped.
        lastResults.clear();
        hasResults = false;
      }
    } else if (line == ":stores") {
      std::cout << "Registered stores on ##/d.stores rank:\n";
      for (const auto &s : coordinator.stores()) {
        std::cout << "  - " << s.label << " (role: " << s.role << ")"
                  << " [master: #"
                  << (s.homeCell ? std::to_string(*s.homeCell) : "none")
                  << ", rep: #"
                  << (s.storeCell ? std::to_string(*s.storeCell) : "none")
                  << ", path: " << (s.path.empty() ? "(in-memory)" : s.path)
                  << "]\n";
      }
    } else if (line.starts_with(":save")) {
      const auto pathStart = line.find_first_not_of(' ', 5);
      if (pathStart == std::string::npos || line.substr(0, 5) != ":save") {
        std::cout << "Usage: :save <new-store-path>\n";
      } else if (!hasResults) {
        std::cerr << "Run a query before saving results.\n";
      } else if (refuseDurableAction()) {
        // Nothing written; the typed code is on stderr.
      } else {
        const auto path = line.substr(pathStart);
        try {
          if (std::filesystem::exists(path)) {
            throw std::invalid_argument("result path already exists: " + path);
          }
          const auto rows =
              resultRows(coordinator, lastResults, captureStore.get(),
                         stdinStore ? stdinStore->path().string() : "");
          xanadu::Store output(permascroll);
          xanadu::writeResultSlice(output, rows);
          output.save(path);
          std::cout << "Saved " << rows.size() << " result rows to " << path
                    << "\n";
        } catch (const std::exception &error) {
          std::cerr << "Save error: " << error.what() << "\n";
        }
      }
    } else if (line.starts_with(":store ")) {
      std::istringstream iss(line);
      std::string cmd, label, path;
      iss >> cmd >> label >> path;
      if (!label.empty() && !path.empty()) {
        coordinator.loadAndAddStore(label, "library", path, permascroll);
        std::cout << "Loaded store '" << label << "' from " << path << "\n";
      } else {
        std::cout << "Usage: :store <label> <path>\n";
      }
    } else if (line.starts_with(":ast ")) {
      std::string q = line.substr(5);
      try {
        xanadu::vql::Parser p(q);
        auto ast = p.parseQuery();
        std::cout << xanadu::vql::AsciiVisualizer::renderAST(ast) << "\n";
      } catch (const std::exception &e) {
        std::cerr << "Parse error: " << e.what() << "\n";
      }
    } else if (line.starts_with(":asm ")) {
      std::string q = line.substr(5);
      try {
        xanadu::vql::VQLCompiler comp(coordinator.core(), vm);
        auto res = comp.compile(q);
        if (res.success) {
          std::cout << res.disassembly << "\n";
        } else {
          std::cerr << "Compilation failed: " << res.errorMessage << "\n";
        }
      } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
      }
    } else {
      runQuery(line);
    }
  }

  return 0;
}

/**
 * @file main.cpp
 * @brief VQL Query Runner and Interactive REPL (vquery).
 *
 * Executes VQL queries against single or multiple xanadoc / slice stores.
 * Supports on-the-fly execution via direct fast-path evaluator (VQLEngine) or
 * by compiling to Vortex bytecode (VQLCompiler) and executing in VortexVM.
 * Features in-place store modification, designated output store
 * creation/update, and ASCII art visualization for terminal debugging.
 */
#include <argparse/argparse.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ascii_visualizer.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/multi_store.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace {

std::string extractStoreLabel(const std::string &path) {
  std::filesystem::path p(path);
  std::string name = p.filename().string();
  if (name.empty() && p.has_parent_path()) {
    name = p.parent_path().filename().string();
  }
  return name.empty() ? "store" : name;
}

void printHelpREPL() {
  std::cout << "VQL Interactive REPL Commands:\n"
            << "  <query>               Execute VQL query expression\n"
            << "  :view [dims]          Toggle visible connection view or set "
               "viewing dimensions\n"
            << "  :dims [dims]          Inspect or set active viewing "
               "dimensions (e.g. d.1,d.2,d.3)\n"
            << "  :ascii                Toggle ASCII art output mode\n"
            << "  :engine <dir|vortex>  Switch execution engine (direct or "
               "vortex)\n"
            << "  :store <label> <path> Load additional xanadoc/slice store\n"
            << "  :stores               List currently loaded stores\n"
            << "  :ast <query>          Print AST dump for query\n"
            << "  :asm <query>          Print compiled bytecode for query\n"
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
  options.push_back(argv[0]);

  const std::vector<std::string> valueOptions = {
      "-e",       "--eval",   "-f",
      "--file",   "-o",       "--output-store",
      "--engine", "--format", "--permascroll",
      "-d",       "--dims",   "--view-dims"};

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
      options.push_back(arg);
      if (i + 1 < argc) {
        options.push_back(argv[++i]);
      }
    } else if (arg.starts_with("-")) {
      options.push_back(arg);
    } else {
      positionals.push_back(arg);
    }
  }

  options.insert(options.end(), positionals.begin(), positionals.end());
  return options;
}

} // namespace

int main(int argc, char *argv[]) {
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

  program.add_argument("--in-place")
      .help("Modify the primary input store in-place")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--engine")
      .help(
          "Execution engine: 'direct' (fast-path C++) or 'vortex' (compile to "
          "bytecode and run in VM)")
      .default_value(std::string("direct"));

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
  std::string format = program.get<std::string>("--format");
  if (program.get<bool>("--ascii")) {
    format = "ascii";
  } else if (program.get<bool>("--view") || program.get<bool>("--grid")) {
    format = "view";
  } else if (program.get<bool>("--text")) {
    format = "text";
  } else if (program.get<bool>("--cells")) {
    format = "cells";
  }

  std::string dimsOpt                   = program.get<std::string>("--dims");
  std::vector<std::string> viewDimNames = parseDimensionNames(dimsOpt);

  std::string engineMode = program.get<std::string>("--engine");
  bool dumpAst           = program.get<bool>("--dump-ast");
  bool dumpAsm           = program.get<bool>("--dump-asm");

  std::vector<std::string> storePaths;
  if (program.present<std::vector<std::string>>("stores")) {
    storePaths = program.get<std::vector<std::string>>("stores");
  }

  // Setup permascroll and MultiStoreCoordinator
  std::string permaDir = program.get<std::string>("--permascroll");
  xanadu::UserPermascroll::Config permaConfig;
  if (!permaDir.empty()) {
    permaConfig.storageDir = permaDir;
  } else if (!storePaths.empty()) {
    std::filesystem::path p(storePaths[0]);
    if (std::filesystem::exists(p / "permascroll")) {
      permaConfig.storageDir = p / "permascroll";
    } else if (p.has_parent_path() &&
               std::filesystem::exists(p.parent_path() / "permascroll")) {
      permaConfig.storageDir = p.parent_path() / "permascroll";
    } else if (p.has_parent_path() && p.parent_path().has_parent_path() &&
               std::filesystem::exists(p.parent_path().parent_path() /
                                       "permascroll")) {
      permaConfig.storageDir = p.parent_path().parent_path() / "permascroll";
    }
  }

  auto permascroll =
      std::make_shared<xanadu::UserPermascroll>(std::move(permaConfig));
  xanadu::vql::MultiStoreCoordinator coordinator;

  std::string primaryPath;
  if (!storePaths.empty()) {
    primaryPath = storePaths[0];
    for (std::size_t i = 0; i < storePaths.size(); ++i) {
      const std::string &path = storePaths[i];
      std::string label       = extractStoreLabel(path);
      std::string role        = (i == 0) ? "primary" : "library";

      if (std::filesystem::exists(path)) {
        coordinator.loadAndAddStore(label, role, path, permascroll);
      } else {
        std::cerr << "Warning: store path does not exist: " << path << "\n";
      }
    }
  }

  // Obtain query text
  std::string queryText = program.get<std::string>("--eval");
  std::string queryFile = program.get<std::string>("--file");

  if (queryText.empty() && !queryFile.empty()) {
    std::ifstream ifs(queryFile);
    if (!ifs) {
      std::cerr << "Error: cannot open query file: " << queryFile << "\n";
      return 1;
    }
    queryText.assign((std::istreambuf_iterator<char>(ifs)),
                     (std::istreambuf_iterator<char>()));
  }

  // Query Execution Helper
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

    std::vector<zigzag::CellRef> results;

    if (engineMode == "vortex") {
      // Compile on-the-fly and execute in VortexVM
      zigzag::vortex::VortexVM vm(coordinator.core());
      xanadu::vql::VQLCompiler compiler(coordinator.core(), vm);

      xanadu::vql::CompilationOptions opts;
      opts.targetLibrary = false;
      auto compRes       = compiler.compile(ast, opts);

      if (!compRes.success) {
        std::cerr << "Compilation failed: " << compRes.errorMessage << "\n";
        return false;
      }

      if (dumpAsm) {
        std::cout << compRes.disassembly << "\n";
      }

      // Execute in VM
      auto cursors = vm.activeCursors();
      if (!cursors.empty()) {
        auto execRes = vm.run(cursors.front(), 100000);
        if (!execRes.success && execRes.errorMessage != "Halted" &&
            execRes.errorMessage != "Finished") {
          std::cerr << "VortexVM execution failed: " << execRes.errorMessage
                    << "\n";
          return false;
        }
      }

      // Collect outputs along cursor or results
      zigzag::CellRef haltOp = zigzag::noCell;
      for (auto it = compRes.generatedOpcodes.rbegin();
           it != compRes.generatedOpcodes.rend(); ++it) {
        auto k = vm.getOpcodeKind(*it);
        if (k && *k == zigzag::vortex::OpcodeKind::Halt) {
          haltOp = *it;
          break;
        }
      }
      if (haltOp != zigzag::noCell) {
        results = coordinator.core().inputsOf(haltOp);
      } else if (!compRes.generatedOpcodes.empty()) {
        results.push_back(compRes.generatedOpcodes.front());
      }
    } else {
      // Direct fast-path execution
      xanadu::vql::VQLEngine engine(coordinator);
      results = engine.execute(ast);
    }

    // Format results
    if (format == "ascii") {
      std::cout << xanadu::vql::AsciiVisualizer::renderQueryResults(
                       coordinator.arena(), results)
                << "\n";
    } else if (format == "view" || format == "grid") {
      std::vector<xanadu::vql::ViewDimension> vdims;
      for (const auto &dname : viewDimNames) {
        vdims.push_back({dname, coordinator.resolveDimension(dname)});
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
        auto val = coordinator.core().render(c);
        if (std::holds_alternative<std::string>(val)) {
          std::cout << std::get<std::string>(val) << "\n";
        } else if (std::holds_alternative<double>(val)) {
          std::cout << std::get<double>(val) << "\n";
        } else if (std::holds_alternative<std::int64_t>(val)) {
          std::cout << std::get<std::int64_t>(val) << "\n";
        } else if (std::holds_alternative<bool>(val)) {
          std::cout << (std::get<bool>(val) ? "true" : "false") << "\n";
        }
      }
    }

    return true;
  };

  // If query is provided, execute batch and handle output persistence
  if (!queryText.empty()) {
    bool ok = runQuery(queryText);
    if (!ok) return 1;

    // Handle in-place mutation
    if (program.get<bool>("--in-place") && !primaryPath.empty()) {
      auto primStore = coordinator.primaryStore();
      if (primStore && primStore->store) {
        primStore->store->save(primaryPath);
        std::cout << "Saved in-place changes to primary store: " << primaryPath
                  << "\n";
      }
    }

    // Handle output store
    std::string outPath = program.get<std::string>("--output-store");
    if (!outPath.empty()) {
      std::filesystem::create_directories(outPath);
      xanadu::Store outStore(permascroll);
      if (std::filesystem::exists(outPath + "/ops.nodes")) {
        outStore.load(outPath);
      }
      zigzag::vortex::VortexVM vm(coordinator.core());
      xanadu::vql::VQLCompiler compiler(coordinator.core(), vm);
      compiler.exportToStore(outStore);
      outStore.save(outPath);
      std::cout << "Result store written to: " << outPath << "\n";
    }

    return 0;
  }

  // Interactive REPL Mode
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
    } else if (line == ":help") {
      printHelpREPL();
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
      if (eng == "direct" || eng == "vortex") {
        engineMode = eng;
        std::cout << "Engine set to: " << engineMode << "\n";
      } else {
        std::cout << "Unknown engine. Use :engine direct or :engine vortex\n";
      }
    } else if (line == ":stores") {
      std::cout << "Registered stores on ##/d.stores rank:\n";
      for (const auto &s : coordinator.stores()) {
        std::cout << "  - " << s.label << " (role: " << s.role << ")"
                  << " [master: #" << s.homeCell << ", rep: #" << s.storeCell
                  << ", path: " << (s.path.empty() ? "(in-memory)" : s.path)
                  << "]\n";
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
        zigzag::vortex::VortexVM vm(coordinator.core());
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

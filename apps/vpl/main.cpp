/**
 * @file main.cpp
 * @brief VPL Query Runner and Interactive REPL (vpl).
 *
 * Executes VPL array expressions using either a direct fast-path evaluator
 * (VPLEngine) or ahead-of-time bytecode compilation into VortexVM
 * (VPLCompiler). Supports dual APL/J syntax, 2D/3D lattice grid visualization
 * (:grid), and persistent store modification.
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
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vpl/parser.hpp"
#include "common/xanadu/vpl/view.hpp"
#include "common/xanadu/vpl/vpl_engine.hpp"
#include "common/xanadu/vql/ascii_visualizer.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vpl;
using namespace zigzag;

std::string formatCell(const ArenaManifold &arena, CellRef c) {
  if (c == noCell) return "·";
  if (auto iVal = arena.asInt64(c)) return std::to_string(*iVal);
  if (auto dVal = arena.asDouble(c)) {
    std::ostringstream ss;
    ss << *dVal;
    return ss.str();
  }
  std::string txt = arena.textOf(c);
  if (!txt.empty()) return txt;
  return "c#" + std::to_string(c);
}

std::string formatView(const VplView &view, const ArenaManifold &arena) {
  if (view.isScalar()) {
    if (view.isString()) return view.scalarString();
    if (view.isFloat()) {
      std::ostringstream ss;
      ss << view.scalarFloat();
      return ss.str();
    }
    return std::to_string(view.scalarInt());
  }

  if (view.isEnclosed() && view.enclosedView()) {
    return "< " + formatView(*view.enclosedView(), arena) + " >";
  }

  auto sh    = view.shape(arena);
  auto cells = view.collectCells(arena);
  if (cells.empty()) return "";

  if (sh.size() <= 1) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (i > 0) oss << " ";
      oss << formatCell(arena, cells[i]);
    }
    return oss.str();
  }

  if (sh.size() == 2) {
    std::size_t rows = sh[0];
    std::size_t cols = sh[1];
    std::ostringstream oss;
    std::size_t idx = 0;
    for (std::size_t r = 0; r < rows; ++r) {
      if (r > 0) oss << "\n";
      for (std::size_t c = 0; c < cols; ++c) {
        if (c > 0) oss << " ";
        if (idx < cells.size()) {
          oss << formatCell(arena, cells[idx++]);
        }
      }
    }
    return oss.str();
  }

  std::ostringstream oss;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if (i > 0) oss << " ";
    oss << formatCell(arena, cells[i]);
  }
  return oss.str();
}

std::string renderGrid(const VplView &view, const ArenaManifold &arena,
                       const vortex::VortexCore &core) {
  auto cells = view.collectCells(arena);
  if (cells.empty()) {
    if (view.origin() != noCell) {
      cells.push_back(view.origin());
    } else {
      return "[ VPL ] Empty view (no cells to visualize).\n";
    }
  }

  std::vector<xanadu::vql::ViewDimension> viewDims;
  if (!view.axes().empty()) {
    for (std::size_t i = 0; i < view.axes().size(); ++i) {
      std::string name   = "d." + std::to_string(i + 1);
      std::string actual = arena.textOf(view.axes()[i].dim);
      if (!actual.empty()) name = actual;
      viewDims.push_back({name, view.axes()[i].dim});
    }
  } else {
    viewDims.push_back({"d.1", core.dims().dims});
    viewDims.push_back({"d.spin", core.dims().spin});
    viewDims.push_back({"d.step", core.dims().step});
  }

  return xanadu::vql::AsciiVisualizer::renderCellConnections(arena, cells,
                                                             viewDims);
}

void printREPLHelp() {
  std::cout
      << "VPL (Vortex Parallel Language) Interactive REPL Commands:\n"
      << "  <expr>                Evaluate VPL expression (APL glyphs or "
         "J-style ASCII)\n"
      << "  :grid / :view         Render ASCII 2D/3D lattice visualization of "
         "current view\n"
      << "  :dims                 Inspect active system and user dimensions\n"
      << "  :engine <dir|vortex>  Switch execution engine ('direct' or "
         "'vortex')\n"
      << "  :ast <expr>           Dump abstract syntax tree for expression\n"
      << "  :asm <expr>           Dump compiled Vortex bytecode disassembly\n"
      << "  :time                 Inspect current hypertime microversion\n"
      << "  :help                 Show this help message\n"
      << "  :quit / :exit / )off  Exit the REPL\n";
}

std::vector<std::string> reorderArgs(int argc, char *argv[]) {
  std::vector<std::string> options;
  std::vector<std::string> positionals;
  options.push_back(argv[0]);

  const std::vector<std::string> valueOptions = {
      "-e",      "--eval",   "-f",       "--file",       "-o", "--output-store",
      "--store", "--engine", "--format", "--permascroll"};

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
  argparse::ArgumentParser program("vpl", "1.0.0");

  program.add_argument("file")
      .help("Path to VPL source script to execute")
      .default_value(std::string(""));

  program.add_argument("-e", "--eval")
      .help("Execute inline VPL expression")
      .default_value(std::string(""));

  program.add_argument("-f", "--file")
      .help("Path to VPL source script (alternative to positional)")
      .default_value(std::string(""));

  program.add_argument("--engine")
      .help("Execution engine: 'direct' (fast-path C++) or 'vortex' (bytecode "
            "VM)")
      .default_value(std::string("direct"));

  program.add_argument("-o", "--output-store")
      .help("Path to result store to create or update")
      .default_value(std::string(""));

  program.add_argument("--store")
      .help("Path to input store directory to load")
      .default_value(std::string(""));

  program.add_argument("--format")
      .help("Output format: 'text', 'ascii', 'grid', 'view'")
      .default_value(std::string("text"));

  program.add_argument("--grid")
      .help("Shorthand for --format grid (2D/3D lattice visualization)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--view")
      .help("Alias for --grid")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--dump-ast")
      .help("Print formatted AST before execution")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--dump-asm")
      .help("Print compiled Vortex bytecode disassembly")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("-O", "--optimize")
      .help("Enable compiler optimization passes in vortex engine mode")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--headless")
      .help("Run headless (no interactive display or prompts)")
      .default_value(false)
      .implicit_value(true);

  try {
    auto args = reorderArgs(argc, argv);
    program.parse_args(args);
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    std::cerr << program;
    return 1;
  }

  std::string vplText = program.get<std::string>("--eval");
  std::string vplFile = program.get<std::string>("file");
  if (vplFile.empty()) {
    vplFile = program.get<std::string>("--file");
  }

  if (vplText.empty() && !vplFile.empty()) {
    std::ifstream ifs(vplFile);
    if (!ifs) {
      std::cerr << "Error: cannot open script file: " << vplFile << "\n";
      return 1;
    }
    vplText.assign((std::istreambuf_iterator<char>(ifs)),
                   (std::istreambuf_iterator<char>()));
  }

  std::string engineMode = program.get<std::string>("--engine");
  std::string format     = program.get<std::string>("--format");
  if (program.get<bool>("--grid") || program.get<bool>("--view")) {
    format = "grid";
  }

  bool dumpAst  = program.get<bool>("--dump-ast");
  bool dumpAsm  = program.get<bool>("--dump-asm");
  bool optimize = program.get<bool>("--optimize");

  ArenaManifold arena;
  vortex::VortexCore core(arena);
  vortex::VortexVM vm(core);
  VPLEngine directEngine(core);
  VPLCompiler compiler(core, vm);

  // Optional store loading
  std::string inputStore = program.get<std::string>("--store");
  std::shared_ptr<xanadu::UserPermascroll> permascroll =
      std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(permascroll);
  if (!inputStore.empty() &&
      std::filesystem::exists(inputStore + "/ops.nodes")) {
    store.load(inputStore);
  }

  // Batch Mode (expression or file provided)
  if (!vplText.empty()) {
    std::shared_ptr<Program> ast;
    try {
      Parser parser(vplText);
      ast = parser.parseProgram();
      if (!ast) {
        std::cerr << "Parse error: received null AST\n";
        return 1;
      }
    } catch (const std::exception &err) {
      std::cerr << "Parse error: " << err.what() << "\n";
      return 1;
    }

    if (dumpAst) {
      std::cout << VPLCompiler::renderAST(*ast) << "\n";
    }

    if (dumpAsm || engineMode == "vortex") {
      CompilationOptions cOpts;
      cOpts.optimize = optimize;
      auto cRes      = compiler.compile(*ast, cOpts);
      if (dumpAsm) {
        std::cout << cRes.disassembly << "\n";
      }
      if (engineMode == "vortex") {
        if (!cRes.success) {
          std::cerr << "Compilation failed: " << cRes.errorMessage << "\n";
          return 1;
        }
        auto cursors = vm.activeCursors();
        if (!cursors.empty()) {
          auto execRes = vm.run(cursors.front(), 100000);
          if (!execRes.success && execRes.errorMessage != "Halted") {
            std::cerr << "Execution error: " << execRes.errorMessage << "\n";
            return 1;
          }
        }
        // Output result of vortex execution
        if (!cRes.generatedOpcodes.empty()) {
          CellRef lastOp = cRes.generatedOpcodes.back();
          auto inCells   = core.inputsOf(lastOp);
          if (!inCells.empty()) {
            std::cout << formatCell(arena, inCells.front()) << "\n";
          }
        }
      }
    }

    if (engineMode == "direct") {
      try {
        VplView result = directEngine.evaluate(*ast);
        if (format == "grid") {
          std::cout << renderGrid(result, arena, core) << "\n";
        } else {
          std::cout << formatView(result, arena) << "\n";
        }
      } catch (const std::exception &err) {
        std::cerr << "Evaluation error: " << err.what() << "\n";
        return 1;
      }
    }

    std::string outStore = program.get<std::string>("--output-store");
    if (!outStore.empty()) {
      std::filesystem::create_directories(outStore);
      compiler.exportToStore(store);
      store.save(outStore);
      std::cout << "Successfully saved to " << outStore << "\n";
    }

    return 0;
  }

  // Interactive REPL Mode
  bool isTty = isatty(STDIN_FILENO) && !program.get<bool>("--headless");
  if (isTty) {
    std::cout << "VPL (Vortex Parallel Language) 1.0.0\n"
              << "Type APL glyphs or J-style ASCII. Type :help for commands.\n";
  }

  VplView lastView;
  std::string line;

  while (true) {
    if (isTty) {
      std::cout << "      " << std::flush;
    }
    if (!std::getline(std::cin, line)) {
      break;
    }

    // Trim leading whitespace
    std::size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
      continue;
    }
    std::string trimmed = line.substr(start);

    // REPL Commands
    if (trimmed == ":quit" || trimmed == ":exit" || trimmed == ")off" ||
        trimmed == ")OFF") {
      break;
    }
    if (trimmed == ":help") {
      printREPLHelp();
      continue;
    }
    if (trimmed == ":grid" || trimmed == ":view") {
      std::cout << renderGrid(lastView, arena, core) << "\n";
      continue;
    }
    if (trimmed == ":dims") {
      std::cout << "Active dimensions:\n"
                << "  d.1, d.2, d.3, d.doc, d.spin, d.step, d.grab, d.clone, "
                   "d.transclude\n";
      continue;
    }
    if (trimmed.starts_with(":engine")) {
      std::string arg    = trimmed.substr(7);
      std::size_t aStart = arg.find_first_not_of(" \t");
      if (aStart != std::string::npos) {
        std::string mode = arg.substr(aStart);
        if (mode == "direct" || mode == "vortex") {
          engineMode = mode;
          std::cout << "Execution engine switched to: " << engineMode << "\n";
        } else {
          std::cout << "Unknown engine '" << mode
                    << "'. Use 'direct' or 'vortex'.\n";
        }
      } else {
        std::cout << "Current execution engine: " << engineMode << "\n";
      }
      continue;
    }
    if (trimmed.starts_with(":ast ")) {
      std::string code = trimmed.substr(5);
      try {
        Parser p(code);
        auto pAst = p.parseProgram();
        if (pAst) {
          std::cout << VPLCompiler::renderAST(*pAst) << "\n";
        }
      } catch (const std::exception &err) {
        std::cerr << "Parse error: " << err.what() << "\n";
      }
      continue;
    }
    if (trimmed.starts_with(":asm ")) {
      std::string code = trimmed.substr(5);
      try {
        Parser p(code);
        auto pAst = p.parseProgram();
        if (pAst) {
          CompilationOptions cOpts;
          cOpts.optimize = optimize;
          auto cRes      = compiler.compile(*pAst, cOpts);
          std::cout << cRes.disassembly << "\n";
        }
      } catch (const std::exception &err) {
        std::cerr << "Compilation error: " << err.what() << "\n";
      }
      continue;
    }
    if (trimmed == ":time") {
      std::cout << "Microversion: head\n";
      continue;
    }

    // Evaluate VPL expression
    try {
      Parser p(trimmed);
      auto pAst = p.parseProgram();
      if (!pAst) continue;

      if (engineMode == "direct") {
        lastView = directEngine.evaluate(*pAst);
        if (format == "grid") {
          std::cout << renderGrid(lastView, arena, core) << "\n";
        } else {
          std::cout << formatView(lastView, arena) << "\n";
        }
      } else {
        CompilationOptions cOpts;
        cOpts.optimize = optimize;
        auto cRes      = compiler.compile(*pAst, cOpts);
        if (!cRes.success) {
          std::cerr << "Compilation error: " << cRes.errorMessage << "\n";
          continue;
        }
        auto cursors = vm.activeCursors();
        if (!cursors.empty()) {
          vm.run(cursors.front(), 100000);
        }
        if (!cRes.generatedOpcodes.empty()) {
          CellRef lastOp = cRes.generatedOpcodes.back();
          auto inCells   = core.inputsOf(lastOp);
          if (!inCells.empty()) {
            std::cout << formatCell(arena, inCells.front()) << "\n";
          }
        }
      }
    } catch (const std::exception &err) {
      std::cerr << "Error: " << err.what() << "\n";
    }
  }

  return 0;
}

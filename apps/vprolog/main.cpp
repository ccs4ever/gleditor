/**
 * @file main.cpp
 * @brief VProlog CLI Runner and Interactive REPL.
 *
 * Provides standalone Prolog execution over the Vortex / Vlog hyperstructural
 * logic runtime. Supports consulting files, evaluating queries, interactive
 * backtracking, listing knowledge base clauses, and rendering visual 2D/3D
 * Zigzag lattice connection topologies.
 */
#include <argparse/argparse.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/xanadu/result_slice.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/store_stream.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/vql/ascii_visualizer.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using namespace xanadu::vprolog;
using namespace zigzag;

void printREPLHelp() {
  std::cout
      << "VProlog Interactive REPL Commands:\n"
      << "  <query>.              Execute query and backtrack through "
         "solutions\n"
      << "  assert(<clause>).     Assert a new fact or rule into the database\n"
      << "  consult('<file>').    Consult and compile a Prolog source file\n"
      << "  listing.              List all asserted clauses in the database\n"
      << "  listing(<pred>).      List clauses for predicate <pred>\n"
      << "  :open <store>         Consult fact rows from a result slice\n"
      << "  :save <file>          Save live clauses as Prolog source\n"
      << "  :export <store>       Save the last query solutions as a result "
         "slice\n"
      << "  :grid                 Display ASCII lattice topology of logic "
         "cells\n"
      << "  :help / help.         Show this help message\n"
      << "  :quit / :exit / halt. Exit the REPL\n";
}

bool consultFile(Compiler &compiler, std::vector<Clause> &clauses,
                 const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open()) {
    std::cerr << "Error: could not open file '" << path << "'\n";
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string content = ss.str();
  try {
    Parser p(content);
    Program prog = p.parseProgram();
    for (const auto &c : prog.clauses) {
      compiler.compileClause(c);
      clauses.push_back(c);
    }
    std::cout << "% Consulted '" << path << "' (" << prog.clauses.size()
              << " clauses)\n";
    return true;
  } catch (const std::exception &ex) {
    std::cerr << "Error parsing '" << path << "': " << ex.what() << "\n";
    return false;
  }
}

std::string
openFactStore(Compiler &compiler, std::vector<Clause> &clauses,
              const std::string &path,
              const std::shared_ptr<xanadu::UserPermascroll> &permascroll,
              const bool streamed = false) {
  xanadu::Store input(permascroll);
  input.load(path);
  const auto sourceName = streamed ? "store:" + input.documentId().str() : path;
  const auto version    = input.primaryCurrentVersion();
  const auto rows       = xanadu::readResultSlice(input, version);
  const auto manifold   = input.rebuildManifold(version);
  const auto resultDim  = manifold.dimensionNamed("d.result", input);
  std::string sources;
  if (resultDim) {
    auto cell = manifold.linked(manifold.home(), *resultDim, DimVector::POS);
    for (std::size_t remaining = manifold.cellCount();
         cell != noCell && remaining-- > 0;
         cell = manifold.linked(cell, *resultDim, DimVector::POS)) {
      if (!sources.empty()) sources += ";";
      sources += sourceName + "#cell=" + std::to_string(cell);
    }
  }
  std::vector<Clause> parsed;
  parsed.reserve(rows.size());
  for (const auto &row : rows) {
    Parser parser(row.text);
    parsed.push_back(parser.parseClause());
  }
  for (const auto &clause : parsed) {
    compiler.compileClause(clause);
    clauses.push_back(clause);
  }
  std::cout << "% Opened " << rows.size() << " facts from " << path << "\n";
  return sources;
}

void printListing(const std::vector<Clause> &clauses,
                  std::string_view filterPred = {}) {
  for (const auto &cl : clauses) {
    if (!filterPred.empty()) {
      std::string fn;
      if (const auto *c = cl.head.asCompound()) {
        fn = c->functor;
      } else if (const auto *a = cl.head.asAtom()) {
        fn = a->name;
      }
      if (fn != filterPred) {
        continue;
      }
    }
    std::cout << formatClause(cl) << "\n";
  }
}

void printGrid(Compiler &compiler) {
  auto preds = compiler.customPredicates();
  if (preds.empty()) {
    std::cout << "[ VProlog ] No custom predicates in database.\n";
    return;
  }
  std::vector<xanadu::vql::ViewDimension> viewDims = {
      {.name = "d.clause", .dim = compiler.core().dims().clause},
      {.name = "d.grab", .dim = compiler.core().dims().grab},
      {.name = "d.spin", .dim = compiler.core().dims().spin},
      {.name = "d.step", .dim = compiler.core().dims().step}};

  std::vector<CellRef> cellsToInspect;
  for (CellRef pred : preds) {
    cellsToInspect.push_back(pred);
    CellRef cl = compiler.core().arena().linked(
        pred, compiler.core().dims().clause, false);
    std::size_t limit = 50;
    while (cl != noCell && limit-- > 0) {
      cellsToInspect.push_back(cl);
      CellRef hd = compiler.core().arena().linked(
          cl, compiler.core().dims().grab, false);
      if (hd != noCell) {
        cellsToInspect.push_back(hd);
      }
      cl = compiler.core().arena().linked(cl, compiler.core().dims().clause,
                                          false);
    }
  }
  std::cout << xanadu::vql::AsciiVisualizer::renderCellConnections(
                   compiler.core().arena(), cellsToInspect, viewDims)
            << "\n";
}

void runREPL(Compiler &compiler, std::vector<Clause> &clauses,
             const std::shared_ptr<xanadu::UserPermascroll> &permascroll) {
  std::cout << "VProlog 0.1.0 (Vortex/Vlog Hyperstructural Logic Engine)\n"
            << "Type ':help' or 'help.' for commands. 'halt.' to exit.\n\n";

  std::string buffer;
  std::vector<xanadu::ResultRow> lastResults;
  bool hasQuery = false;
  std::string inputProvenance;
  while (true) {
    if (buffer.empty()) {
      std::cout << "?- " << std::flush;
    } else {
      std::cout << "|  " << std::flush;
    }

    std::string line;
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }

    auto endTrim = line.find_last_not_of(" \t\r\n");
    if (endTrim == std::string::npos) {
      continue;
    }
    line = line.substr(0, endTrim + 1);

    if (buffer.empty()) {
      if (line == ":quit" || line == ":exit" || line == "halt." ||
          line == "halt") {
        break;
      }
      if (line == ":help" || line == "help." || line == "help") {
        printREPLHelp();
        continue;
      }
      if (line == ":listing" || line == "listing." || line == "listing") {
        printListing(clauses);
        continue;
      }
      if (line.starts_with("listing(") && line.ends_with(").")) {
        std::string pred = line.substr(8, line.size() - 10);
        printListing(clauses, pred);
        continue;
      }
      if (line == ":grid") {
        printGrid(compiler);
        continue;
      }
      if (line.starts_with(":open ")) {
        const auto path = line.substr(6);
        try {
          inputProvenance = openFactStore(compiler, clauses, path, permascroll);
        } catch (const std::exception &error) {
          std::cerr << "Open error: " << error.what() << "\n";
        }
        continue;
      }
      if (line.starts_with(":save ")) {
        const auto path = line.substr(6);
        if (std::filesystem::exists(path)) {
          std::cerr << "Save error: path already exists: " << path << "\n";
        } else {
          std::ofstream output(path);
          if (!output) {
            std::cerr << "Save error: cannot open " << path << "\n";
          } else {
            for (const auto &clause : clauses) {
              output << formatClause(clause) << "\n";
            }
            output.close();
            if (output) {
              std::cout << "% Saved " << clauses.size() << " clauses to "
                        << path << "\n";
            } else {
              std::cerr << "Save error: write failed for " << path << "\n";
            }
          }
        }
        continue;
      }
      if (line.starts_with(":export ")) {
        const auto path = line.substr(8);
        try {
          if (!hasQuery) {
            throw std::invalid_argument("run a query before exporting");
          }
          if (std::filesystem::exists(path)) {
            throw std::invalid_argument("result path already exists: " + path);
          }
          xanadu::Store output(permascroll);
          xanadu::writeResultSlice(output, lastResults);
          output.save(path);
          std::cout << "% Exported " << lastResults.size() << " solutions to "
                    << path << "\n";
        } catch (const std::exception &error) {
          std::cerr << "Export error: " << error.what() << "\n";
        }
        continue;
      }
      if (line.starts_with(":consult ")) {
        std::string path = line.substr(9);
        consultFile(compiler, clauses, path);
        continue;
      }
      if (line.starts_with("consult(") && line.ends_with(").")) {
        std::string path = line.substr(8, line.size() - 10);
        if (path.size() >= 2 &&
            ((path.front() == '\'' && path.back() == '\'') ||
             (path.front() == '"' && path.back() == '"'))) {
          path = path.substr(1, path.size() - 2);
        }
        consultFile(compiler, clauses, path);
        continue;
      }
      if ((line.starts_with("assert(") || line.starts_with("assertz(")) &&
          line.ends_with(").")) {
        std::size_t prefixLen = line.starts_with("assert(") ? 7 : 8;
        std::string clauseStr =
            line.substr(prefixLen, line.size() - prefixLen - 2) + ".";
        try {
          Parser cp(clauseStr);
          Clause cl = cp.parseClause();
          compiler.compileClause(cl);
          clauses.push_back(cl);
          std::cout << "true.\n";
        } catch (const std::exception &ex) {
          std::cerr << "Error: " << ex.what() << "\n";
        }
        continue;
      }
    }

    buffer += (buffer.empty() ? "" : " ") + line;

    if (!buffer.empty() && buffer.back() == '.') {
      std::string input = buffer;
      buffer.clear();

      try {
        CompiledQuery q = compiler.compileQuery(input);
        auto sols       = compiler.solve(q, 100);
        lastResults.clear();
        hasQuery = true;
        for (const auto &sol : sols) {
          std::string value;
          for (std::size_t vi = 0; vi < q.variables.size(); ++vi) {
            const auto &name = q.variables[vi].first;
            const auto found = sol.formatted.find(name);
            if (vi > 0) value += ", ";
            if (q.variables.size() > 1) value += name + " = ";
            value += found != sol.formatted.end() ? found->second : "_";
          }
          if (q.variables.empty()) value = "true";
          lastResults.push_back(
              {.text = std::move(value), .source = inputProvenance});
        }

        if (sols.empty()) {
          std::cout << "false.\n";
        } else {
          if (q.variables.empty()) {
            std::cout << "true.\n";
          } else {
            for (std::size_t i = 0; i < sols.size(); ++i) {
              const auto &sol = sols[i];
              std::string out;
              for (std::size_t vi = 0; vi < q.variables.size(); ++vi) {
                if (vi > 0) out += ", ";
                const auto &varName = q.variables[vi].first;
                auto it             = sol.formatted.find(varName);
                std::string val =
                    (it != sol.formatted.end()) ? it->second : "_";
                out += varName;
                out += " = ";
                out += val;
              }
              std::cout << out;

              if (i + 1 < sols.size()) {
                if (isatty(STDIN_FILENO)) {
                  std::cout << " " << std::flush;
                  std::string resp;
                  std::getline(std::cin, resp);
                  if (!resp.empty() && resp != ";" && resp != " ") {
                    break;
                  }
                } else {
                  std::cout << " ;\n";
                }
              } else {
                std::cout << ".\n";
              }
            }
          }
        }
      } catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << "\n";
      }
    }
  }
}

std::vector<std::string> reorderArgs(int argc, char *argv[]) {
  std::vector<std::string> options;
  std::vector<std::string> positionals;
  options.emplace_back(argv[0]);

  const std::vector<std::string> valueOptions = {
      "-e",           "--eval",          "-q", "--query",
      "-m",           "--max-solutions", "-o", "--output-store",
      "--permascroll"};

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
        options.emplace_back(argv[++i]);
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
  argparse::ArgumentParser program("vprolog", "0.1.0");
  program.add_description(
      "VProlog — Hyperstructural Logic Engine (Nelsonian Prolog on Vortex / "
      "Vlog)");

  program.add_argument("-e", "--eval")
      .help("Evaluate a Prolog query and exit")
      .default_value(std::string(""));

  program.add_argument("-q", "--query")
      .help("Alias for --eval")
      .default_value(std::string(""));

  program.add_argument("-m", "--max-solutions")
      .help("Maximum number of solutions to compute")
      .scan<'i', int>()
      .default_value(100);

  program.add_argument("-o", "--output-store")
      .help("Save query solutions as a result store; '-' writes native store "
            "tar to stdout")
      .default_value(std::string(""));

  program.add_argument("-t", "--top-level")
      .help("Enter interactive REPL even after evaluating files")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--grid")
      .help("Display ASCII visualizer of the logic database manifold")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--listing")
      .help("List loaded predicates and clauses before exiting")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--headless")
      .help("Run without touching display devices")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--permascroll")
      .help("Separate user permascroll directory (created if absent)")
      .default_value(std::string(""));

  program.add_argument("files")
      .help("Prolog source files (.pl / .vlog) to consult")
      .remaining();

  auto reordered = reorderArgs(argc, argv);
  try {
    program.parse_args(reordered);
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << program;
    return 1;
  }

  ArenaManifold arena;
  vortex::VortexCore core(arena);
  Compiler compiler(core);
  std::vector<Clause> loadedClauses;
  const auto permascrollPath = program.get<std::string>("--permascroll");
  std::shared_ptr<xanadu::UserPermascroll> permascroll =
      permascrollPath.empty()
          ? xanadu::PermascrollRegistry::instance().defaultUser()
          : std::make_shared<xanadu::UserPermascroll>(
                xanadu::UserPermascroll::Config{.storageDir = permascrollPath});
  std::unique_ptr<xanadu::TemporaryStreamStore> stdinStore;
  std::string inputProvenance;
  const auto outputStore  = program.get<std::string>("--output-store");
  const bool streamOutput = outputStore == "-";
  auto *originalOutput    = std::cout.rdbuf();
  if (streamOutput) std::cout.rdbuf(std::cerr.rdbuf());

  if (program.is_used("files")) {
    auto files = program.get<std::vector<std::string>>("files");
    for (const auto &file : files) {
      try {
        std::string path = file;
        if (file == "-") {
          if (stdinStore)
            throw std::invalid_argument("only one stdin store allowed");
          stdinStore = std::make_unique<xanadu::TemporaryStreamStore>(std::cin);
          path       = stdinStore->path().string();
        }
        if (std::filesystem::is_directory(path)) {
          inputProvenance = openFactStore(compiler, loadedClauses, path,
                                          permascroll, file == "-");
        } else if (!consultFile(compiler, loadedClauses, path)) {
          return 1;
        }
      } catch (const std::exception &error) {
        std::cerr << "Open error: " << error.what() << "\n";
        return 1;
      }
    }
  }

  if (program.get<bool>("--listing")) {
    printListing(loadedClauses);
  }

  auto evalQuery = program.get<std::string>("-e");
  if (evalQuery.empty()) {
    evalQuery = program.get<std::string>("-q");
  }

  const bool enterRepl = program.get<bool>("-t") || evalQuery.empty();
  if (streamOutput && enterRepl) {
    std::cerr << "-o - requires a batch query without --top-level.\n";
    return 1;
  }

  if (!evalQuery.empty()) {
    try {
      CompiledQuery query = compiler.compileQuery(evalQuery);
      const int maxSols   = program.get<int>("-m");
      auto solutions = compiler.solve(query, static_cast<std::size_t>(maxSols));
      std::unique_ptr<xanadu::TemporaryStreamStore> streamStore;
      std::filesystem::path destination;
      if (!outputStore.empty()) {
        if (!streamOutput && std::filesystem::exists(outputStore)) {
          throw std::invalid_argument("result path already exists: " +
                                      outputStore);
        }
        std::vector<xanadu::ResultRow> rows;
        rows.reserve(solutions.size());
        for (const auto &solution : solutions) {
          std::string value;
          for (std::size_t i = 0; i < query.variables.size(); ++i) {
            const auto &name = query.variables[i].first;
            if (i > 0) value += ", ";
            if (query.variables.size() > 1) value += name + " = ";
            const auto found = solution.formatted.find(name);
            value += found != solution.formatted.end() ? found->second : "_";
          }
          if (query.variables.empty()) value = "true";
          rows.push_back({.text = std::move(value), .source = inputProvenance});
        }
        if (streamOutput) {
          streamStore = std::make_unique<xanadu::TemporaryStreamStore>();
        }
        destination = streamOutput ? streamStore->path()
                                   : std::filesystem::path(outputStore);
        xanadu::Store output(permascroll);
        xanadu::writeResultSlice(output, rows);
        output.save(destination.string());
      }

      if (solutions.empty()) {
        std::cout << "false.\n";
        if (program.get<bool>("--grid")) {
          printGrid(compiler);
        }
      } else {
        if (query.variables.empty()) {
          std::cout << "true.\n";
        } else {
          for (std::size_t i = 0; i < solutions.size(); ++i) {
            const auto &sol = solutions[i];
            std::string out;
            for (std::size_t vi = 0; vi < query.variables.size(); ++vi) {
              if (vi > 0) out += ", ";
              const auto &varName = query.variables[vi].first;
              auto it             = sol.formatted.find(varName);
              std::string val = (it != sol.formatted.end()) ? it->second : "_";
              out += varName;
              out += " = ";
              out += val;
            }
            std::cout << out;
            if (i + 1 < solutions.size()) {
              std::cout << " ;\n";
            } else {
              std::cout << ".\n";
            }
          }
        }
        if (program.get<bool>("--grid")) {
          printGrid(compiler);
        }
      }
      if (streamOutput) {
        std::cout.flush();
        std::cout.rdbuf(originalOutput);
        xanadu::writeStoreStream(destination, std::cout);
      }
      if (solutions.empty() && !enterRepl) return 1;
    } catch (const std::exception &ex) {
      std::cerr << "Error: " << ex.what() << "\n";
      return 1;
    }
  }

  if (enterRepl) {
    runREPL(compiler, loadedClauses, permascroll);
  }

  return 0;
}

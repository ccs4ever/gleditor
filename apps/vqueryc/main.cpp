/**
 * @file main.cpp
 * @brief Standalone VQL Compiler CLI (vqueryc).
 *
 * Compiles VQL query expressions into Vortex bytecode on an ArenaManifold or
 * Store. Supports executable store targets (home/d.spin ending in Halt) and
 * library store targets (home/d.stdlib with exported symbol names ending in
 * Return), as well as AST dumps and disassembled opcode listings.
 */
#include <argparse/argparse.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ascii_visualizer.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/parser.hpp"

int main(int argc, char *argv[]) {
  argparse::ArgumentParser program("vqueryc", "1.0.0");

  program.add_argument("query_file")
      .help("Path to VQL query file")
      .default_value(std::string(""));

  program.add_argument("-e", "--eval")
      .help("Compile inline query string")
      .default_value(std::string(""));

  program.add_argument("-o", "--output")
      .help("Path to output store directory")
      .default_value(std::string(""));

  program.add_argument("--executable")
      .help("Produce an executable store (home/d.spin) [default]")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--library")
      .help("Produce a library store with module name (home/d.stdlib)")
      .default_value(std::string(""));

  program.add_argument("--symbol")
      .help("Exported symbol name in library mode")
      .default_value(std::string("query"));

  program.add_argument("--dump-ast")
      .help("Print formatted AST representation")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--dump-asm")
      .help("Disassemble generated Vortex bytecode")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("-O", "--optimize")
      .help("Enable compiler optimization passes")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--headless")
      .help("Run headless (no display)")
      .default_value(false)
      .implicit_value(true);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    std::cerr << program;
    return 1;
  }

  std::string queryText = program.get<std::string>("--eval");
  std::string queryFile = program.get<std::string>("query_file");

  if (queryText.empty() && !queryFile.empty()) {
    std::ifstream ifs(queryFile);
    if (!ifs) {
      std::cerr << "Error: cannot open query file: " << queryFile << "\n";
      return 1;
    }
    queryText.assign((std::istreambuf_iterator<char>(ifs)),
                     (std::istreambuf_iterator<char>()));
  }

  if (queryText.empty()) {
    std::cerr << "Error: no query specified. Provide a query file or use -e "
                 "\"<query>\".\n";
    return 1;
  }

  // Parse AST
  xanadu::vql::QueryExpression ast;
  try {
    xanadu::vql::Parser parser(queryText);
    ast = parser.parseQuery();
  } catch (const xanadu::vql::ParseError &err) {
    std::cerr << "Parse error: " << err.what() << "\n";
    return 1;
  }

  if (program.get<bool>("--dump-ast")) {
    std::cout << xanadu::vql::AsciiVisualizer::renderAST(ast) << "\n";
  }

  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  xanadu::vql::VQLCompiler compiler(core, vm);

  xanadu::vql::CompilationOptions options;
  std::string libName = program.get<std::string>("--library");
  if (!libName.empty()) {
    options.targetLibrary = true;
    options.moduleName    = libName;
  } else {
    options.targetLibrary = false;
  }
  options.symbolName = program.get<std::string>("--symbol");
  options.optimize   = program.get<bool>("--optimize");

  auto result = compiler.compile(ast, options);
  if (!result.success) {
    std::cerr << "Compilation failed: " << result.errorMessage << "\n";
    return 1;
  }

  if (program.get<bool>("--dump-asm")) {
    std::cout << result.disassembly << "\n";
  }

  std::string outPath = program.get<std::string>("--output");
  if (!outPath.empty()) {
    std::filesystem::create_directories(outPath);
    auto permascroll = std::make_shared<xanadu::UserPermascroll>();
    xanadu::Store store(permascroll);
    if (std::filesystem::exists(outPath + "/ops.nodes")) {
      store.load(outPath);
    }
    compiler.exportToStore(store);
    store.save(outPath);
    std::cout << "Successfully compiled and saved to " << outPath << "\n";
  }

  return 0;
}

/**
 * @file main.cpp
 * @brief Standalone vicon compiler (viconc).
 *
 * Compiles a vicon program without running it: parses, lowers, and reports
 * the entry opcode of the compiled stream. Mirrors apps/vlacec.
 */
#include <argparse/argparse.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "common/xanadu/vicon/compiler.hpp"
#include "common/xanadu/vicon/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

std::string readFile(const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open()) {
    throw std::runtime_error("could not open file '" + path + "'");
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

} // namespace

// Catches std::exception and reports it; anything else (a real bug, not a
// user-facing failure) is deliberately left to terminate with a backtrace
// rather than be swallowed into a generic error message.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[]) {
  argparse::ArgumentParser program("viconc", "0.1.0");
  program.add_description("viconc — vicon compiler");

  program.add_argument("file")
      .help("Path to vicon source file")
      .default_value(std::string(""));

  program.add_argument("-e", "--eval")
      .help("Compile an inline vicon expression")
      .default_value(std::string(""));

  program.add_argument("--emit")
      .help("Print the compiled entry opcode and exported procedures")
      .default_value(false)
      .implicit_value(true);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << program;
    return 1;
  }

  std::string source = program.get<std::string>("-e");
  if (source.empty()) {
    const auto file = program.get<std::string>("file");
    if (file.empty()) {
      std::cerr << "viconc: no input file (use -e for inline source)\n";
      return 1;
    }
    try {
      source = readFile(file);
    } catch (const std::exception &e) {
      std::cerr << e.what() << "\n";
      return 1;
    }
  }

  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  xanadu::vicon::Compiler compiler(core);

  auto compiled = compiler.compileSource(source);
  if (!compiled) {
    const auto &f = compiled.error();
    std::cerr << "viconc: " << f.loc.line << ":" << f.loc.column << ": "
              << static_cast<int>(f.code) << ": " << f.detail << "\n";
    return 1;
  }
  if (program.get<bool>("--emit")) {
    std::cout << "entry: cell " << compiled->entry << "\n";
    std::cout << "result cell: " << compiled->resultCell << "\n";
    for (const auto &[name, op] : compiled->procedures) {
      std::cout << "procedure " << name << ": cell " << op << "\n";
    }
  }
  return 0;
}

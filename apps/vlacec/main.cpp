/**
 * @file main.cpp
 * @brief Standalone vlace compiler / type-checker (vlacec).
 *
 * Parses and type-checks a vlace program through the shared Driver. On success
 * it prints the inferred type; with --emit it prints the rendered value. On
 * failure it prints the formatted error and exits nonzero. The type-check pass
 * is normative here and only skipped with --no-typecheck.
 */
#include <argparse/argparse.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "common/xanadu/vlace/driver.hpp"

namespace {

using xanadu::vlace::Driver;

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
  argparse::ArgumentParser program("vlacec", "0.1.0");
  program.add_description("vlacec — vlace compiler and type-checker");

  program.add_argument("file")
      .help("Path to vlace source file")
      .default_value(std::string(""));

  program.add_argument("-e", "--eval")
      .help("Compile an inline vlace expression")
      .default_value(std::string(""));

  program.add_argument("--emit")
      .help("Also print the rendered value, not only the inferred type")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--no-typecheck")
      .help("Skip the normative Hindley-Milner type-check pass")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--headless")
      .help("Run headless (no display)")
      .default_value(false)
      .implicit_value(true);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n" << program;
    return 1;
  }

  auto source     = program.get<std::string>("--eval");
  const auto file = program.get<std::string>("file");
  if (source.empty() && !file.empty()) {
    try {
      source = readFile(file);
    } catch (const std::exception &err) {
      std::cerr << "Error: " << err.what() << "\n";
      return 1;
    }
  }

  if (source.empty()) {
    std::cerr << "Error: no vlace source specified. Provide a source file or "
                 "use -e \"<expr>\".\n";
    return 1;
  }

  Driver driver;
  driver.withTypeChecking(!program.get<bool>("--no-typecheck"));

  auto result = driver.run(source);
  if (!result) {
    std::cerr << Driver::formatError(result.error()) << "\n";
    return 1;
  }

  std::cout << result->type << "\n";
  if (program.get<bool>("--emit")) {
    std::cout << result->value << "\n";
  }
  return 0;
}

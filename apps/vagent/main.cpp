/**
 * @file main.cpp
 * @brief vagent command line: static checking now, running in iteration 3.
 *
 * `check` is the whole of the front end the language has so far: lex, parse
 * and check, with every diagnostic printed as file:line:col so an editor can
 * jump to it. `run` is reserved so the command line does not change shape when
 * the compiler and adapter land.
 */
#include <argparse/argparse.hpp>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/parser.hpp"

namespace {

using namespace xanadu::vagent;

// Shell convention: 1 is "the program is wrong", 2 is "could not be asked".
constexpr int exitOk    = 0;
constexpr int exitError = 1;
constexpr int exitUsage = 2;

std::optional<std::string> readFile(const std::string &path) {
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

int runCheck(const argparse::ArgumentParser &command) {
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

} // namespace

int main(int argc, char **argv) {
  argparse::ArgumentParser program("vagent", "1.0.0");
  program.add_description("vagent agent-oriented language: checker and runner");

  argparse::ArgumentParser checkCommand("check");
  checkCommand.add_description("Lex, parse and check source files");
  checkCommand.add_argument("files")
      .help("vagent source files")
      .nargs(argparse::nargs_pattern::at_least_one);

  argparse::ArgumentParser runCommand("run");
  runCommand.add_description("Run an agent from a source file (iteration 3)");
  runCommand.add_argument("file").help("vagent source file");
  runCommand.add_argument("--agent").help("agent type to start").required();

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
    std::cerr << "vagent: run is not built yet (iteration 3)\n";
    return exitUsage;
  }
  std::cerr << program;
  return exitUsage;
}

/**
 * @file main.cpp
 * @brief vicon CLI runner and interactive REPL.
 *
 * Runs vicon programs and expressions over the Vortex runtime: every
 * expression is a generator; results print one per line as the generator is
 * resumed. Mirrors apps/vprolog/main.cpp.
 */
#include <argparse/argparse.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "common/xanadu/vicon/driver.hpp"

namespace {

using xanadu::vicon::Driver;

std::string readFile(const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open()) {
    throw std::runtime_error("could not open file '" + path + "'");
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void printHelp() {
  std::cout
      << "vicon Interactive REPL Commands:\n"
      << "  <expr>               Compile and run; results print one per line\n"
      << "  every <expr>         Drive a generator to exhaustion\n"
      << "  :load <file>         Run a vicon source file\n"
      << "  :help                Show this help\n"
      << "  :quit                Exit\n";
}

void printResults(Driver &driver) {
  driver.every([](std::string_view text) { std::cout << text << "\n"; });
}

void runRepl(Driver &driver) {
  std::cout << "vicon 0.1.0 (goal-directed evaluation on Vortex)\n"
            << "Type ':help' for commands. ':quit' to exit.\n\n";
  while (true) {
    std::cout << ">> " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }
    auto last = line.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) continue;
    line = line.substr(0, last + 1);
    if (line == ":quit" || line == ":exit") break;
    if (line == ":help") {
      printHelp();
      continue;
    }
    if (line.starts_with(":load ")) {
      try {
        line = readFile(line.substr(6));
      } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        continue;
      }
    }
    auto started = driver.start(line);
    if (!started) {
      std::cerr << Driver::formatError(started.error()) << "\n";
      continue;
    }
    printResults(driver);
  }
}

} // namespace

// Catches std::exception and reports it; anything else (a real bug, not a
// user-facing failure) is deliberately left to terminate with a backtrace
// rather than be swallowed into a generic error message.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[]) {
  argparse::ArgumentParser program("vicon", "0.1.0");
  program.add_description(
      "vicon — Icon-shaped goal-directed language on the Vortex runtime");

  program.add_argument("-e", "--eval")
      .help("Evaluate a vicon expression/program and exit")
      .default_value(std::string(""));

  program.add_argument("-t", "--top-level")
      .help("Enter the interactive REPL even after evaluating files")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--headless")
      .help("Run without touching display devices")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("files")
      .help("vicon source files (.icn / .vicon) to run")
      .remaining();

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << program;
    return 1;
  }

  Driver driver;
  const auto eval = program.get<std::string>("-e");
  const auto files = program.get<std::vector<std::string>>("files");

  for (const auto &file : files) {
    std::string source;
    try {
      source = file == "-" ? ([&] {
                 std::stringstream ss;
                 ss << std::cin.rdbuf();
                 return ss.str();
               })()
                           : readFile(file);
    } catch (const std::exception &e) {
      std::cerr << e.what() << "\n";
      return 1;
    }
    auto results = driver.runAll(source);
    if (!results) {
      std::cerr << Driver::formatError(results.error()) << "\n";
      return 1;
    }
    for (const auto &r : *results) std::cout << r << "\n";
  }

  if (!eval.empty()) {
    auto results = driver.runAll(eval);
    if (!results) {
      std::cerr << Driver::formatError(results.error()) << "\n";
      return 1;
    }
    for (const auto &r : *results) std::cout << r << "\n";
    return 0;
  }

  if (program.get<bool>("-t") || (files.empty() && eval.empty())) {
    runRepl(driver);
  }
  return 0;
}

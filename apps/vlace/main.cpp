/**
 * @file main.cpp
 * @brief vlace CLI runner and interactive REPL.
 *
 * Evaluates vlace functional-language programs through the shared Driver:
 * parse, optionally type-check (normative Hindley-Milner), evaluate, and
 * render. A positional FILE or -e runs once; otherwise an interactive REPL
 * reads a line at a time and prints "<value> : <type>" or a formatted error.
 */
#include <argparse/argparse.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "common/xanadu/vlace/driver.hpp"
#include "common/xanadu/vortex/mesh_cli.hpp"
#include "common/xanadu/zigzag/lens_repl.hpp"

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

// Runs a single source string once. If the program's type is an IO action, it
// is PERFORMED at the boundary (console wired to stdin/stdout) and the final
// wrapped value is printed; otherwise the pure value is rendered. Returns the
// process exit code.
int runOnce(Driver &driver, const std::string &source) {
  auto result = driver.run(source);
  if (!result) {
    std::cerr << Driver::formatError(result.error()) << "\n";
    return 1;
  }
  // An IO-typed program is a description; perform it (§6.5). The type is
  // "IO ..." when type-checking ran; when it is off, fall back to detecting the
  // rendered description's IO. prefix.
  const bool isIO =
      result->type.rfind("IO ", 0) == 0 || result->type == "IO" ||
      (result->type.empty() && result->value.rfind("IO.", 0) == 0);
  if (isIO) {
    auto done = driver.runIO(source, std::cin, std::cout);
    if (!done) {
      std::cerr << Driver::formatError(done.error()) << "\n";
      return 1;
    }
    return 0;
  }
  std::cout << result->value << "\n";
  return 0;
}

void runREPL(Driver &driver) {
  std::cout << "vlace 0.1.0 (functional language on Vortex)\n"
            << "Type an expression; :quit, :q, :exit or EOF to leave.\n\n";

  std::string line;
  zigzag::LensRepl lensRepl;
  while (true) {
    std::cout << "vlace> " << std::flush;
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }

    const auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
      continue;
    }
    const auto end     = line.find_last_not_of(" \t\r\n");
    const auto trimmed = line.substr(start, end - start + 1);

    if (trimmed == ":quit" || trimmed == ":q" || trimmed == ":exit") {
      break;
    }
    // vlace holds no open store, so meta: has no fold to bind -- the
    // reply says so.
    if (zigzag::LensRepl::handles(trimmed)) {
      lensRepl.run(trimmed, std::cout);
      continue;
    }

    auto result = driver.run(trimmed);
    if (!result) {
      std::cout << Driver::formatError(result.error()) << "\n";
      continue;
    }
    std::cout << result->value << " : " << result->type << "\n";
  }
}

} // namespace

// Catches std::exception and reports it; anything else (a real bug, not a
// user-facing failure) is deliberately left to terminate with a backtrace
// rather than be swallowed into a generic error message.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[]) {
  if (const auto mesh = zigzag::vortex::mesh::runMeshCommand(
          zigzag::vortex::mesh::Frontend::Vlace, argc, argv))
    return *mesh;
  argparse::ArgumentParser program("vlace", "0.1.0");
  program.add_description("vlace — a lazy functional language on Vortex");

  program.add_argument("file")
      .help("vlace source file to run")
      .default_value(std::string(""));

  program.add_argument("-e", "--eval")
      .help("Evaluate a vlace expression and exit")
      .default_value(std::string(""));

  program.add_argument("--lens")
      .help("Open a lens URI (path, dir:, git:, sql:, cfg:, log:, journal:, "
            "union:) and print its projected tree")
      .default_value(std::string{});
  program.add_argument("--headless")
      .help("Run the file or -e expression and exit (no interactive prompt)")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--no-typecheck")
      .help("Skip the normative Hindley-Milner type-check pass")
      .default_value(false)
      .implicit_value(true);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << program;
    return 1;
  }

  {
    const auto lensUri = program.get<std::string>("--lens");
    if (!lensUri.empty()) {
      return zigzag::runLensRaster(lensUri, std::cout);
    }
  }

  Driver driver;
  driver.withTypeChecking(!program.get<bool>("--no-typecheck"));

  const auto evalExpr = program.get<std::string>("-e");
  const auto file     = program.get<std::string>("file");

  if (!evalExpr.empty()) {
    return runOnce(driver, evalExpr);
  }

  if (!file.empty()) {
    try {
      return runOnce(driver, readFile(file));
    } catch (const std::exception &err) {
      std::cerr << "Error: " << err.what() << "\n";
      return 1;
    }
  }

  // No source given. --headless with nothing to run is a no-op success; a
  // bare invocation drops into the REPL.
  if (program.get<bool>("--headless")) {
    return 0;
  }

  runREPL(driver);
  return 0;
}

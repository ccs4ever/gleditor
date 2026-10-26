#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/token.hpp"

namespace {

void usage(std::ostream &out) {
  out << "Usage: vagent check FILE...\n"
         "       vagent --help\n"
         "       vagent --version\n\n"
         "Check each standalone vagent source unit for syntax and static "
         "errors.\n"
         "Use - for standard input. Cross-file module loading, Vortex "
         "lowering,\n"
         "agent execution and the REPL are not implemented in this "
         "iteration.\n";
}

bool checkFile(std::string_view path) {
  std::string source;
  if (path == "-") {
    source.assign(std::istreambuf_iterator<char>(std::cin), {});
    if (std::cin.bad()) {
      std::cerr << "vagent: cannot read standard input\n";
      return false;
    }
  } else {
    std::ifstream input{std::string(path)};
    if (!input) {
      std::cerr << path << ": cannot open source file\n";
      return false;
    }
    source.assign(std::istreambuf_iterator<char>(input), {});
    if (input.bad()) {
      std::cerr << path << ": cannot read source file\n";
      return false;
    }
  }

  xanadu::vagent::Parser parser(source);
  auto program = parser.parseProgram();
  if (!program) {
    std::cerr << path << ':'
              << xanadu::vagent::formatDiagnostic(program.error()) << '\n';
    return false;
  }
  auto checked = xanadu::vagent::check(*program);
  if (!checked) {
    for (const auto &diagnostic : checked.error()) {
      std::cerr << path << ':' << xanadu::vagent::formatDiagnostic(diagnostic)
                << '\n';
    }
    return false;
  }
  std::cout << path << ": checked\n";
  return true;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage(std::cout);
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << "vagent 0.1.0 (frontend iteration)\n";
    return 0;
  }
  if (argc < 3 || std::string_view(argv[1]) != "check") {
    usage(std::cerr);
    return 2;
  }
  try {
    bool valid = true;
    for (int index = 2; index < argc; ++index) {
      valid = checkFile(argv[index]) && valid;
    }
    return valid ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "vagent: " << error.what() << '\n';
    return 1;
  }
}

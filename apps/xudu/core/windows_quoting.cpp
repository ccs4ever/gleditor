/**
 * @file windows_quoting.cpp
 * @brief Implementation of the pure half of Windows command-line quoting.
 */
#include "windows_quoting.hpp" // IWYU pragma: associated

namespace xudu {

std::wstring quoteWindowsArgument(const std::wstring &arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return arg;
  }

  std::wstring out(1, L'"');
  std::size_t backslashes = 0;
  for (const auto ch : arg) {
    if (L'\\' == ch) {
      backslashes++;
      continue;
    }
    if (L'"' == ch) {
      out.append((backslashes * 2) + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(backslashes, L'\\');
      out.push_back(ch);
    }
    backslashes = 0;
  }
  // A run of trailing backslashes is doubled too: it is about to meet the
  // closing quote, which is exactly the case the escaping exists for.
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

} // namespace xudu

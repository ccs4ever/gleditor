/**
 * @file windows_quoting.hpp
 * @brief Turning one argument into one token of a Windows command line.
 *
 * CreateProcess takes a single string rather than an argument vector, so
 * spawning a program without a shell -- see provenance.cpp's run() -- means
 * building that string by hand. This is the part of that job with no
 * dependency on Windows itself: given a wide string, it says what a program
 * reading it back with CommandLineToArgvW would see as one argument. The
 * platform-specific half, converting UTF-8 to UTF-16, stays beside the
 * Windows API calls that need it.
 *
 * Kept apart from them for one reason: this is the part that is easy to get
 * subtly wrong -- a trailing backslash, a literal quote -- and the part that
 * is possible to check without a Windows machine to run it on.
 */
#ifndef XUDU_WINDOWS_QUOTING_H
#define XUDU_WINDOWS_QUOTING_H

#include <string>

namespace xudu {

/**
 * @brief Quote @p arg as one argument of a Windows command line, if it needs
 *        it.
 *
 * The documented inverse of CommandLineToArgvW's own parsing: a run of
 * backslashes is only special immediately before a quote, where it must be
 * doubled, and a literal quote inside an argument is always escaped. Returned
 * unquoted when it holds none of the characters that would otherwise split it
 * -- an empty argument still has to be quoted, since an empty token is not
 * the same as no token at all.
 */
[[nodiscard]] std::wstring quoteWindowsArgument(const std::wstring &arg);

} // namespace xudu

#endif // XUDU_WINDOWS_QUOTING_H

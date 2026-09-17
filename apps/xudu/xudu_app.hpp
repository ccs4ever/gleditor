#pragma once

namespace xanadu {}
namespace xudu {
using namespace ::xanadu;

/**
 * @brief Main application entry point for the xanadoc editor.
 *
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @param enableXuzz When true, enables the Zigzag visualizer and presentation boundary (xuzz mode).
 * @return Process exit code.
 */
int runXuduApp(int argc, char **argv, bool enableXuzz = false);

} // namespace xudu

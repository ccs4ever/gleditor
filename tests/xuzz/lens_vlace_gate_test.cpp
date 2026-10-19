/**
 * @file lens_vlace_gate_test.cpp
 * @brief Frontend gate: the vlace functional language, driven in-process, over
 *        a directory projected through the anything-to-ZigZag FilesystemLens.
 *
 * The point of this gate is that a *projected* source is usable from vlace as
 * directly as a native slice: a vlace program calls `openLens` on a real
 * directory, folds over the projected cells with `childrenAll`, tokenizes their
 * text with `words`, and computes a WORD-FREQUENCY HISTOGRAM -- no native-store
 * setup, no C++ cell authoring, just the IO entry point the REPL/batch runner
 * share (Driver::runIO).
 *
 * WHAT TEXT IS COUNTED, AND WHY (ground truth today):
 *   The FilesystemLens sets each cell's *content* to the directory entry's
 *   NAME, not the file's bytes (projecting file bytes would need
 * importFileStore and is not wired through the lens yet). So `cellText child`
 * yields the file name. To make a histogram with real repetition we name the
 * fixture files with multi-word names (spaces in the name), e.g. "red red
 * blue"; `words` then splits each name into tokens and we count tokens across
 * every child. This is the most honest demonstration available now: a frequency
 * count of the tokens the lens actually exposes (entry names), computed
 * end-to-end by a vlace program over projected cells.
 *
 * The histogram is rendered as a bar chart ("token ###") so no Int-to-String
 * primitive is needed (vlace has none): the count becomes a run of '#'. Bars
 * are emitted in a program-fixed key order (red, green, blue) because vlace has
 * no string ordering; Int IS ordered, but we don't even need that here.
 */
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/vlace/driver.hpp"

namespace {

using xanadu::vlace::Driver;

// Build a deterministic fixture directory of multi-word file names.
//
//   root/
//     "blue green"      -> tokens: blue green
//     "red green green" -> tokens: red green green
//     "red red blue"    -> tokens: red red blue
//
// childrenAll walks d.contains + d.seq, and the FilesystemLens sorts entries,
// so the children arrive in name order; the token multiset across all three is
// therefore stable regardless of traversal:
//   red:   2 (red red blue) + 1 (red green green)          = 3
//   green: 1 (blue green)   + 2 (red green green)          = 3
//   blue:  1 (blue green)   + 1 (red red blue)             = 2
std::filesystem::path makeFixture() {
  auto base = std::filesystem::temp_directory_path() /
              ("vlace_lens_" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  auto touch = [](const std::filesystem::path &p) {
    std::ofstream f(p);
    f << "x"; // content is irrelevant; the lens exposes the NAME as cell text
  };
  touch(base / "blue green");
  touch(base / "red green green");
  touch(base / "red red blue");
  return base;
}

// Run a vlace IO program with no stdin, returning {ok, stdout, error-text}.
struct Run {
  bool ok{false};
  std::string out;
  std::string err;
};

Run runProgram(const std::string &src) {
  Driver d;
  d.withTypeChecking(false); // inline (prelude-less) helpers; evaluate only
  std::istringstream in;
  std::ostringstream out;
  auto r = d.runIO(src, in, out);
  Run result;
  result.out = out.str();
  if (r) {
    result.ok = true;
  } else {
    result.err = Driver::formatError(r.error());
  }
  return result;
}

// The vlace histogram program as a single source string. `%s` is replaced with
// the fixture directory path. Decls first, then the trailing IO expression.
//
//   map/foldr/append : list basics (inline; the Driver prepends no prelude)
//   concatMap        : flatten the per-name token lists into one token list
//   count            : how many list elements equal a given token (String ==)
//   bar              : render a count as that many '#' (no Int->String needed)
//   line             : "token ###\n" for one key
//   render           : concatenate a line per key, in the fixed key order
//
// Entry: openLens the directory, take the root, then over the children:
//   map cellText     -> list of entry NAMES
//   concatMap words  -> every token across every name
//   render ... keys  -> the histogram text, printed with putStr
constexpr const char *kProgramFmt =
    "map f xs = case xs of [] => [] ; (h : t) => f h : map f t ;\n"
    "foldr f z xs = case xs of [] => z ; (h : t) => f h (foldr f z t) ;\n"
    "append xs ys = case xs of [] => ys ; (h : t) => h : append t ys ;\n"
    "concatMap f xs = foldr (fn x => fn acc => append (f x) acc) [] xs ;\n"
    "count tok xs = foldr (fn x => fn acc => case x == tok of "
    "True => acc + 1 ; False => acc) 0 xs ;\n"
    "bar n = case n of 0 => \"\" ; _ => \"#\" ++ bar (n - 1) ;\n"
    "line toks tok = tok ++ \" \" ++ bar (count tok toks) ++ \"\\n\" ;\n"
    "render toks keys = foldr (fn k => fn acc => line toks k ++ acc) \"\" "
    "keys ;\n"
    "openLens \"%s\" >>= (fn root => "
    "putStr (render (concatMap words (map cellText (childrenAll root))) "
    "[\"red\", \"green\", \"blue\"]))";

std::string programFor(const std::filesystem::path &dir) {
  std::string fmt = kProgramFmt;
  auto pos        = fmt.find("%s");
  fmt.replace(pos, 2, dir.string());
  return fmt;
}

// The histogram the program must print: keys in fixed order, counts as bars.
//   red   = 3  -> "###"
//   green = 3  -> "###"
//   blue  = 2  -> "##"
constexpr const char *kExpectedHistogram = "red ###\ngreen ###\nblue ##\n";

TEST(LensVlaceGate, WordFrequencyHistogramOverProjectedDirectory) {
  auto dir  = makeFixture();
  auto prog = programFor(dir);
  auto r    = runProgram(prog);
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(r.ok) << "vlace program failed: " << r.err
                    << "\n--- program ---\n"
                    << prog;
  EXPECT_EQ(r.out, kExpectedHistogram);
}

// A sharper statement of the same gate: the program really walked the projected
// cells (three children). If openLens/childrenAll returned nothing, every count
// would be zero and the bars empty; a non-empty histogram proves the fold ran
// over projected cell text.
TEST(LensVlaceGate, HistogramIsNonEmptyProvingCellsWereWalked) {
  auto dir  = makeFixture();
  auto prog = programFor(dir);
  auto r    = runProgram(prog);
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_NE(r.out.find("red ###"), std::string::npos);
  EXPECT_NE(r.out.find("blue ##"), std::string::npos);
  EXPECT_GT(r.out.size(), 0u);
}

} // namespace

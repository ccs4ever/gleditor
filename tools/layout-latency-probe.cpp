/**
 * @file layout-latency-probe.cpp
 * @brief What the loader actually spends before the first page can be shown.
 *
 * Doc::makePages already hands each page to the render thread as soon as it is
 * laid out, so pages are not batched up and delivered at the end. The question
 * this answers is what page *one* costs, and in particular whether it costs
 * something proportional to the whole document rather than to a page.
 *
 * The suspicion is the loop's call to pango_layout_set_text, which is given the
 * entire remaining text on every page:
 *
 *     pango_layout_set_text(lay->gobj(), txt + tSize, text.bytes() - tSize);
 *
 * The layout is bounded by set_height and ellipsized, so it *shows* one page.
 * Whether it *processes* one page's worth is a question about Pango, and the
 * only honest way to answer it is to measure it.
 *
 * Reports, for a range of document sizes: reading and validating the text, then
 * the first page's layout when handed the whole text against a bounded slice of
 * it. Same font, width, height and ellipsize mode as the loader uses.
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <fontconfig/fontconfig.h>
#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::duration<double, std::milli>;

/// The page geometry Doc::makePages uses, so this measures that page.
constexpr double pageHeightMm = 139.70 * 11;
constexpr double pageWidthMm  = 139.70 * 8.5;

/// Prose rather than one long line, so line breaking has realistic work to do.
std::string proseOf(const std::size_t bytes) {
  static const std::string sentence =
      "The quick brown fox jumped over the lazy dog, and then considered "
      "at some length whether the exercise had been worth the trouble. ";
  std::string out;
  out.reserve(bytes + sentence.size());
  while (out.size() < bytes) {
    out += sentence;
    if (0 == (out.size() / sentence.size()) % 7) {
      out += "\n\n";
    }
  }
  out.resize(bytes);
  return out;
}

double timeFirstPage(const gleditor::text::FontFacePtr &font,
                     const char *rawText, const std::size_t bytes,
                     int &linesOut, std::uint32_t &consumedOut) {
  gleditor::text::LayoutOptions opts{
      .maxWidthPx      = static_cast<float>(pageWidthMm),
      .maxHeightPx     = static_cast<float>(pageHeightMm),
      .singleParagraph = false,
      .ellipsize       = true,
  };
  const auto start = Clock::now();
  auto shaping     = gleditor::text::TextLayout::layoutPage(
      std::string_view{rawText, bytes}, font, opts);
  linesOut    = static_cast<int>(shaping.lineCount);
  consumedOut = static_cast<std::uint32_t>(shaping.limit);
  return Ms(Clock::now() - start).count();
}

/// Median of several runs, so one scheduling hiccup does not decide anything.
double median(std::vector<double> runs) {
  std::ranges::sort(runs);
  return runs[runs.size() / 2];
}

} // namespace

int main(const int argc, char **argv) {
  FcInit();
  const std::string fontName = argc > 1 ? argv[1] : "Monospace 16";
  auto font = gleditor::text::FontManager::instance().getFont(fontName);

  // Warm the font machinery up before anything is timed
  {
    const auto warm = proseOf(64 * 1024);
    int lines{};
    std::uint32_t consumed{};
    for (int i = 0; i < 3; i++) {
      static_cast<void>(
          timeFirstPage(font, warm.c_str(), warm.size(), lines, consumed));
    }
  }

  constexpr int repeats = 5;
  std::cout << "font: " << fontName << ", page " << pageWidthMm << "x"
            << pageHeightMm << "mm, median of " << repeats << "\n\n"
            << std::left << std::setw(11) << "doc bytes" << std::setw(13)
            << "page1 time" << std::setw(8) << "lines" << "page holds\n";

  for (const std::size_t size :
       {std::size_t{16} * 1024, std::size_t{64} * 1024, std::size_t{256} * 1024,
        std::size_t{1024} * 1024, std::size_t{4} * 1024 * 1024}) {
    const auto raw = proseOf(size);

    int wholeLines{};
    std::uint32_t wholeHolds{};
    std::vector<double> wholes;
    for (int i = 0; i < repeats; i++) {
      wholes.push_back(
          timeFirstPage(font, raw.c_str(), raw.size(), wholeLines, wholeHolds));
    }

    std::cout << std::left << std::setw(11) << size << std::setw(13)
              << std::fixed << std::setprecision(2) << median(wholes)
              << std::setw(8) << wholeLines << wholeHolds << "\n";
  }

  return 0;
}

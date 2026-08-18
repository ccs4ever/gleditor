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

#include <glib.h>
#include <glibmm/convert.h>
#include <glibmm/ustring.h>
#include <pango/pangocairo.h>
#include <pangomm/cairofontmap.h>
#include <pangomm/init.h>
#include <pangomm/layout.h>

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

Glib::RefPtr<Pango::Layout> pageLayout(const Glib::RefPtr<Pango::Context> &ctx,
                                       const Pango::FontDescription &font) {
  auto lay = Pango::Layout::create(ctx);
  lay->set_font_description(font);
  lay->set_single_paragraph_mode(false);
  lay->set_height(static_cast<int>(std::ceil(pageHeightMm * PANGO_SCALE)));
  lay->set_width(static_cast<int>(std::ceil(pageWidthMm * PANGO_SCALE)));
  lay->set_ellipsize(Pango::EllipsizeMode::END);
  return lay;
}

/// The measurement the loader's first page makes: set the text, then ask the
/// question consumedBytes() asks. get_line_count() is what forces Pango to
/// break lines, so the cost lands here rather than at set_text.
double timeFirstPage(const Glib::RefPtr<Pango::Context> &ctx,
                     const Pango::FontDescription &font, const char *text,
                     const std::size_t bytes, int &linesOut,
                     std::uint32_t &consumedOut) {
  auto lay         = pageLayout(ctx, font);
  const auto start = Clock::now();
  pango_layout_set_text(lay->gobj(), text, static_cast<int>(bytes));
  linesOut         = lay->get_line_count();
  const auto taken = Ms(Clock::now() - start).count();

  // What Doc::consumedBytes would decide this page holds.
  if (linesOut > 0) {
    const auto &last   = lay->get_const_line(linesOut - 1);
    const int len      = last->get_length();
    const auto whole   = static_cast<std::uint32_t>(last->get_start_index() +
                                                    (0 == len ? 1 : len));
    const auto upToCut = static_cast<std::uint32_t>(last->get_start_index());
    consumedOut =
        !lay->is_ellipsized() ? whole : (0 == upToCut ? whole : upToCut);
  }
  return taken;
}

/// Median of several runs, so one scheduling hiccup does not decide anything.
double median(std::vector<double> runs) {
  std::ranges::sort(runs);
  return runs[runs.size() / 2];
}

} // namespace

int main(const int argc, char **argv) {
  Pango::init();
  const std::string fontName = argc > 1 ? argv[1] : "Monospace 16";
  const auto font            = Pango::FontDescription(fontName);
  const auto fonts           = Pango::CairoFontMap::get_default();
  const auto ctx             = fonts->create_context();
  ctx->set_font_description(font);

  // A slice big enough that a page cannot need more than this. Chosen from the
  // page's own capacity rather than guessed: whatever the first page consumes
  // when handed everything, it can be handed a little more than that instead.
  constexpr std::size_t sliceBytes = 8 * 1024;

  // Warm the font machinery up before anything is timed: the first layout in a
  // process pays for fontconfig, and charging that to a document size would be
  // measuring the wrong thing entirely.
  {
    const auto warm = proseOf(64 * 1024);
    int lines{};
    std::uint32_t consumed{};
    for (int i = 0; i < 3; i++) {
      static_cast<void>(
          timeFirstPage(ctx, font, warm.c_str(), warm.size(), lines, consumed));
    }
  }

  constexpr int repeats = 5;
  std::cout << "font: " << fontName << ", page " << pageWidthMm << "x"
            << pageHeightMm << "mm, slice " << sliceBytes
            << " bytes, median of " << repeats << "\n\n"
            << std::left << std::setw(11) << "doc bytes" << std::setw(9)
            << "copy" << std::setw(10) << "validate" << std::setw(13)
            << "page1 whole" << std::setw(13) << "page1 slice" << std::setw(8)
            << "lines" << "page holds\n";

  for (const std::size_t size :
       {std::size_t{16} * 1024, std::size_t{64} * 1024, std::size_t{256} * 1024,
        std::size_t{1024} * 1024, std::size_t{4} * 1024 * 1024}) {
    const auto raw = proseOf(size);

    // What Doc's constructor does before any layout happens, split in two: the
    // source hands back a std::string by value which is assigned into a
    // ustring, and then the whole of it is validated.
    std::vector<double> copies;
    std::vector<double> validates;
    for (int i = 0; i < repeats; i++) {
      const auto copyStart = Clock::now();
      Glib::ustring text   = raw;
      copies.push_back(Ms(Clock::now() - copyStart).count());

      const auto validStart = Clock::now();
      auto iter             = text.begin();
      if (!text.validate(iter)) {
        text = text.make_valid();
      }
      validates.push_back(Ms(Clock::now() - validStart).count());
    }

    const Glib::ustring text = raw;
    int wholeLines{};
    int sliceLines{};
    std::uint32_t wholeHolds{};
    std::uint32_t sliceHolds{};
    std::vector<double> wholes;
    std::vector<double> slices;
    for (int i = 0; i < repeats; i++) {
      wholes.push_back(timeFirstPage(ctx, font, text.raw().c_str(),
                                     text.bytes(), wholeLines, wholeHolds));
      slices.push_back(timeFirstPage(ctx, font, text.raw().c_str(),
                                     std::min(sliceBytes, text.bytes()),
                                     sliceLines, sliceHolds));
    }

    std::cout << std::left << std::setw(11) << size << std::setw(9)
              << std::fixed << std::setprecision(2) << median(copies)
              << std::setw(10) << median(validates) << std::setw(13)
              << median(wholes) << std::setw(13) << median(slices)
              << std::setw(8) << wholeLines << wholeHolds
              << (wholeHolds == sliceHolds ? " (slice agrees)"
                                           : "  SLICE DISAGREES")
              << "\n";
  }

  std::cout
      << "\ncopy and validate both happen before the first page is\n"
         "attempted. page1 is set_text plus get_line_count, which is what\n"
         "consumedBytes() forces. \"page holds\" is the byte count\n"
         "consumedBytes() would return -- if the slice agrees, bounding\n"
         "the text handed to a layout changes no output.\n";
  return 0;
}

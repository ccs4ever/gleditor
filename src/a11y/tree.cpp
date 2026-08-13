/**
 * @file tree.cpp
 * @brief The parts of describing a UI that are arithmetic.
 *
 * Counting characters, counting words, and finding a node in a list. All of it
 * is here rather than spread across the elements that describe themselves,
 * because every one of them would otherwise be counting UTF-8 by hand -- and
 * the count has to agree with what the caret does, or a screen reader reports
 * the cursor somewhere other than where it is drawn.
 */
#include <gleditor/a11y/tree.hpp> // IWYU pragma: associated

#include <algorithm>
#include <cstddef>

namespace gleditor::a11y {

namespace {

/// Bytes in the character beginning at @p at, by its lead byte. A byte that
/// begins nothing valid counts as one: text that is not quite UTF-8 still has
/// to be describable, and refusing to describe it would lose the whole
/// document rather than one character of it.
std::size_t characterAt(const std::string_view text, const std::size_t at) {
  const auto lead         = static_cast<unsigned char>(text[at]);
  const std::size_t bytes = lead < 0x80U   ? 1U
                            : lead < 0xC0U ? 1U
                            : lead < 0xE0U ? 2U
                            : lead < 0xF0U ? 3U
                                           : 4U;
  // A sequence running off the end is as long as what is left of the string.
  return std::min(bytes, text.size() - at);
}

/// Whether a character is one that separates words. Only the ASCII ones: this
/// is a word boundary for moving a cursor, not a Unicode segmentation, and the
/// platforms' own text fields draw the same line.
bool separates(const std::string_view text, const std::size_t at,
               const std::size_t bytes) {
  if (1 != bytes) {
    return false;
  }
  const auto lead = static_cast<unsigned char>(text[at]);
  return ' ' == lead || '\t' == lead || '\n' == lead || '\r' == lead;
}

} // namespace

const Node *Tree::find(const std::uint64_t id) const {
  const auto found = std::ranges::find(nodes, id, &Node::id);
  return nodes.end() == found ? nullptr : &*found;
}

Node &Builder::add(const std::uint64_t local, const Role role) {
  Node node;
  node.id   = id(local);
  node.role = role;
  tree.nodes.push_back(std::move(node));
  return tree.nodes.back();
}

std::vector<std::uint8_t> characterLengths(const std::string_view text) {
  std::vector<std::uint8_t> lengths;
  for (std::size_t at = 0; at < text.size();) {
    const auto bytes = characterAt(text, at);
    lengths.push_back(static_cast<std::uint8_t>(bytes));
    at += bytes;
  }
  return lengths;
}

std::vector<std::uint8_t> wordStarts(const std::string_view text) {
  std::vector<std::uint8_t> starts;
  if (text.empty()) {
    return starts;
  }
  // The first character of a run always begins a word, whatever it is: that is
  // what the platforms assume, and it is what makes a line of leading
  // whitespace a word a cursor can move across rather than a gap in the count.
  starts.push_back(0);

  std::size_t index = 0;
  bool afterSpace   = false;
  for (std::size_t at = 0; at < text.size();) {
    const auto bytes = characterAt(text, at);
    const bool space = separates(text, at, bytes);
    if (!space && afterSpace && 0 != index) {
      // A word begins where the whitespace before it ends, so the space stays
      // with the word it follows and "next word" lands on a word.
      //
      // Silently dropped past what a byte holds. Runs are cut short of that
      // -- see runBreaks -- so this is only reached by text that arrived here
      // some other way, and it costs word navigation in the tail of one run
      // rather than putting a boundary somewhere wrong.
      if (index <= 0xFFU) {
        starts.push_back(static_cast<std::uint8_t>(index));
      }
    }
    afterSpace = space;
    index++;
    at += bytes;
  }
  return starts;
}

std::vector<std::size_t> runBreaks(const std::string_view text) {
  std::vector<std::size_t> breaks{0};
  /// Characters since the run began, and where the last gap in it was.
  std::size_t inRun          = 0;
  std::size_t afterSeparator = 0;
  std::size_t charsThere     = 0;

  for (std::size_t at = 0; at < text.size();) {
    const auto bytes = characterAt(text, at);
    const auto lead  = static_cast<unsigned char>(text[at]);
    const bool space = separates(text, at, bytes);
    at += bytes;
    inRun++;
    if (space) {
      afterSeparator = at;
      charsThere     = inRun;
    }

    if (1 == bytes && '\n' == lead) {
      // The break belongs to the line it ends, so the run stops after it.
      breaks.push_back(at);
      inRun          = 0;
      afterSeparator = 0;
      continue;
    }
    if (inRun < runLimit) {
      continue;
    }
    // At the last gap when there was one in the final quarter, and here when
    // there was not: cutting a word in half is worse than a short line, and a
    // run of two hundred characters with no space in it has nowhere better to
    // be cut than where it got too long.
    const bool useGap =
        afterSeparator > breaks.back() && charsThere > (runLimit * 3 / 4);
    const auto cut = useGap ? afterSeparator : at;
    breaks.push_back(cut);
    at             = cut;
    inRun          = 0;
    afterSeparator = 0;
  }

  if (breaks.back() != text.size()) {
    breaks.push_back(text.size());
  }
  // Empty text is one empty run: a document with nothing in it still has a
  // place the caret can be, and a document with no runs at all has nowhere to
  // report it.
  if (1 == breaks.size()) {
    breaks.push_back(0);
  }
  return breaks;
}

std::size_t characterIndexOf(const std::string_view text,
                             const std::size_t byteOffset) {
  std::size_t index = 0;
  for (std::size_t at = 0; at < text.size();) {
    const auto next = at + characterAt(text, at);
    // Only a character that ends at or before the offset has been passed. One
    // the offset falls inside is the character the offset is in, which is what
    // an index has to name: an offset in the middle of a two-byte letter is
    // that letter, not the one after it.
    if (next > byteOffset) {
      break;
    }
    at = next;
    index++;
  }
  return index;
}

} // namespace gleditor::a11y

// vi: set sw=2 sts=2 ts=2 et:

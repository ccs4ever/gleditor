/**
 * @file documents.cpp
 * @brief Implementation of the document half of the accessibility tree.
 */
#include <gleditor/a11y/documents.hpp> // IWYU pragma: associated

#include <algorithm>
#include <limits>
#include <utility>

#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>

namespace gleditor::a11y {

namespace {

/// Mix a value into a running signature. Nothing cryptographic is wanted: this
/// only has to move when what it is made of moves.
void mix(std::uint64_t &into, const std::uint64_t value) {
  into = (into * 1099511628211ULL) ^ value;
}

/**
 * @brief A world point as a pixel in the window, or nothing.
 *
 * Nothing when the point is behind the camera, where the perspective divide
 * turns into a reflection: a document behind the reader would otherwise be
 * reported as a rectangle somewhere in front of them.
 */
std::optional<std::pair<double, double>> toWindow(const glm::mat4 &transform,
                                                  const glm::vec3 &point,
                                                  const int width,
                                                  const int height) {
  const auto clip = transform * glm::vec4(point, 1.0F);
  if (clip.w <= 0.0F) {
    return std::nullopt;
  }
  const auto ndcX = clip.x / clip.w;
  const auto ndcY = clip.y / clip.w;
  // Y flips: the platforms count pixels from the top of the window and the
  // glyph pipeline draws from the bottom.
  return std::pair{(ndcX * 0.5 + 0.5) * width,
                   (1.0 - (ndcY * 0.5 + 0.5)) * height};
}

/// The rectangle in window pixels covering every page of @p doc that can be
/// projected at all.
std::optional<Rect> boundsOf(const Doc &doc, const glm::mat4 &viewProjection,
                             const int width, const int height) {
  auto left   = std::numeric_limits<double>::max();
  auto top    = std::numeric_limits<double>::max();
  auto right  = std::numeric_limits<double>::lowest();
  auto bottom = std::numeric_limits<double>::lowest();
  bool any    = false;

  for (std::size_t index = 0; index < doc.numPages(); index++) {
    const auto *const page = doc.page(index);
    if (nullptr == page) {
      continue;
    }
    // Both corners, then the extremes of whatever came back: which of the two
    // is the top in window pixels depends on the camera, and taking the
    // minimum is the same answer without having to know.
    const std::array corners{
        std::pair{page->leftPixels(), page->topPixels()},
        std::pair{page->leftPixels() + page->widthPixels(),
                  page->topPixels() + page->heightPixels()}};
    for (const auto &[cornerX, cornerY] : corners) {
      const auto world =
          doc.worldPoint(static_cast<std::uint32_t>(index), cornerX, cornerY);
      if (!world) {
        continue;
      }
      const auto pixel = toWindow(viewProjection, *world, width, height);
      if (!pixel) {
        continue;
      }
      left   = std::min(left, pixel->first);
      right  = std::max(right, pixel->first);
      top    = std::min(top, pixel->second);
      bottom = std::max(bottom, pixel->second);
      any    = true;
    }
  }
  if (!any) {
    return std::nullopt;
  }
  return Rect{left, top, right, bottom};
}

/// Node numbering within this source. A document gets a block of its own, so
/// that a run's id says which document it is in and adding a document does not
/// renumber another's runs.
constexpr std::uint64_t perDocument = 1ULL << 32U;
constexpr std::uint64_t documentId(const std::uint32_t index) {
  return static_cast<std::uint64_t>(index) * perDocument;
}
constexpr std::uint64_t firstRunId(const std::uint32_t index) {
  return documentId(index) + 1;
}

} // namespace

void DocumentsSource::observe(const RenderState &state,
                              const Caret *const caret,
                              const glm::mat4 &viewProjection, const int width,
                              const int height) {
  // What the description would be made of, without making it. Bounds are in
  // here quantised to a few pixels: a document being moved really has changed
  // where it is and an assistive technology should be told, but rebuilding
  // every text run on every frame of an animation would be paying a document's
  // worth of work for a document's worth of motion.
  constexpr double boundsQuantum = 8.0;
  std::uint64_t signature        = state.docs.size();
  for (const auto &doc : state.docs) {
    mix(signature, doc->editGeneration());
    mix(signature, doc->contents().bytes());
    if (const auto rect = boundsOf(*doc, viewProjection, width, height); rect) {
      mix(signature, static_cast<std::uint64_t>(static_cast<std::int64_t>(
                         rect->left / boundsQuantum)));
      mix(signature, static_cast<std::uint64_t>(
                         static_cast<std::int64_t>(rect->top / boundsQuantum)));
      mix(signature, static_cast<std::uint64_t>(static_cast<std::int64_t>(
                         rect->right / boundsQuantum)));
      mix(signature, static_cast<std::uint64_t>(static_cast<std::int64_t>(
                         rect->bottom / boundsQuantum)));
    }
  }
  if (nullptr != caret && caret->active()) {
    mix(signature, caret->documentIndex());
    mix(signature, caret->byteOffset());
    mix(signature, caret->hasSelection() ? caret->selectionStart() : 0);
    mix(signature, caret->hasSelection() ? caret->selectionEnd() : 0);
  }

  if (everObserved && signature == lastSignature) {
    return;
  }
  lastSignature = signature;
  everObserved  = true;
  revision++;

  docs.clear();
  docs.reserve(state.docs.size());
  for (std::uint32_t index = 0; index < state.docs.size(); index++) {
    const auto &doc = state.docs[index];
    Described described;
    described.index  = index;
    described.name   = doc->name();
    described.text   = doc->contents().raw();
    described.breaks = runBreaks(described.text);
    described.bounds = boundsOf(*doc, viewProjection, width, height);
    if (nullptr != caret && caret->active() &&
        caret->documentIndex() == index) {
      described.hasCaret       = true;
      described.caretByte      = caret->byteOffset();
      described.hasSelection   = caret->hasSelection();
      described.selectionStart = caret->selectionStart();
      described.selectionEnd   = caret->selectionEnd();
    }
    docs.push_back(std::move(described));
  }
}

TextPoint DocumentsSource::pointAt(const Described &doc,
                                   const std::uint64_t firstRun,
                                   const std::uint32_t byteOffset) {
  // runBreaks always yields at least two entries -- a start and an end -- so
  // there is always a run for a position to be in, even in an empty document.
  const auto runs = doc.breaks.size() - 1;
  for (std::size_t run = 0; run < runs; run++) {
    const auto start = doc.breaks[run];
    const auto end   = doc.breaks[run + 1];
    // The last run also owns the position at the very end of the text, which
    // is where a caret typed to the end of a document sits.
    if (byteOffset < end || run + 1 == runs) {
      const std::string_view text{doc.text};
      const auto within = std::min<std::size_t>(byteOffset, end) - start;
      return TextPoint{
          firstRun + run,
          characterIndexOf(text.substr(start, end - start), within)};
    }
  }
  return TextPoint{firstRun, 0};
}

void DocumentsSource::describe(Builder &into) {
  for (const auto &doc : docs) {
    const auto id    = documentId(doc.index);
    const auto first = into.id(firstRunId(doc.index));
    const auto runs  = doc.breaks.size() - 1;

    {
      // A document is text somebody types into, so it is described as one
      // rather than as a Document -- which the platforms treat as a page to be
      // read. The caret and the selection would have nowhere to go on a
      // document that could not be edited.
      auto &node     = into.add(id, Role::MultilineTextInput);
      node.label     = doc.name.empty() ? "document" : doc.name;
      node.bounds    = doc.bounds;
      node.focusable = true;
      node.actions   = bit(Action::Focus) | bit(Action::Click);
      node.children.reserve(runs);
      for (std::size_t run = 0; run < runs; run++) {
        node.children.push_back(first + run);
      }
      if (doc.hasCaret) {
        // A selection's anchor is where the drag began and its focus is where
        // the caret is; with nothing selected the two are the same place,
        // which is how a caret is spelled.
        const auto focus = pointAt(doc, first, doc.caretByte);
        node.selection   = TextSelection{
            doc.hasSelection ? pointAt(doc, first,
                                       doc.selectionStart == doc.caretByte
                                             ? doc.selectionEnd
                                             : doc.selectionStart)
                               : focus,
            focus};
        into.takeFocus(into.id(id));
      }
    }

    for (std::size_t run = 0; run < runs; run++) {
      const auto start = doc.breaks[run];
      const auto text =
          std::string_view{doc.text}.substr(start, doc.breaks[run + 1] - start);
      auto &node = into.add(firstRunId(doc.index) + run, Role::TextRun);
      node.value = text;
      node.characterLengths = characterLengths(text);
      node.wordStarts       = wordStarts(text);
    }

    into.contribute(into.id(id));
  }
}

std::uint64_t DocumentsSource::accessibilityRevision() const {
  return revision;
}

bool DocumentsSource::performAction(const std::uint64_t nodeId,
                                    const Action action,
                                    const std::string_view /*value*/) {
  // Which document, and which run of it, from the numbering above.
  const auto local    = Ids::localOf(nodeId);
  const auto index    = static_cast<std::uint32_t>(local / perDocument);
  const auto withinIt = local % perDocument;

  switch (action) {
  case Action::Focus:
  case Action::Click: {
    // Both mean the same thing to a document: put the caret in it -- at the
    // start of the run, when the request was about one, which is what clicking
    // a line does. Only the numbering is worked out here. Turning a run into a
    // byte offset means reading the description, and the description belongs
    // to the render thread; see takeWanted().
    Asked asked;
    asked.document = index;
    if (0 != withinIt) {
      asked.run = static_cast<std::size_t>(withinIt - 1);
    }
    const std::lock_guard locker(wantedGuard);
    wanted.push_back(asked);
    return true;
  }
  case Action::ScrollIntoView:
  case Action::SetValue:
    // Neither is anything this can do yet. Refused rather than ignored, so
    // that a platform asking is told rather than left believing it worked.
    return false;
  }
  return false;
}

std::vector<DocumentsSource::Wanted> DocumentsSource::takeWanted() {
  std::vector<Asked> asked;
  {
    const std::lock_guard locker(wantedGuard);
    asked.swap(wanted);
  }

  std::vector<Wanted> taken;
  taken.reserve(asked.size());
  for (const auto &one : asked) {
    const auto found = std::ranges::find(docs, one.document, &Described::index);
    if (docs.end() == found) {
      // Asked about a document that has since been closed. Nothing to do, and
      // nothing wrong: the platform's tree is a moment behind ours.
      continue;
    }
    Wanted want;
    want.kind     = Wanted::Kind::PlaceCaret;
    want.document = one.document;
    // The document node itself means its start; a run means the first byte of
    // that run.
    want.byteOffset = one.run && *one.run + 1 < found->breaks.size()
                          ? static_cast<std::uint32_t>(found->breaks[*one.run])
                          : 0U;
    taken.push_back(want);
  }
  return taken;
}

} // namespace gleditor::a11y

// vi: set sw=2 sts=2 ts=2 et:

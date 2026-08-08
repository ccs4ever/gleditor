#include "session.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

namespace xudu {

Session::Session(std::string aStorePath) : storePath(std::move(aStorePath)) {
  docStore.load(storePath);
}

MicroversionId Session::versionOf(const std::uint32_t docIndex) const {
  return docIndex < open.size() ? open[docIndex].version : MicroversionId{};
}

void Session::viewOpened(const MicroversionId &version) {
  open.push_back(OpenView{version, docStore.rebuild(version)});
}

void Session::viewClosed(const std::uint32_t docIndex) {
  if (docIndex < open.size()) {
    open.erase(open.begin() + static_cast<std::ptrdiff_t>(docIndex));
  }
}

void Session::viewMovedTo(const std::uint32_t docIndex,
                          const MicroversionId &version) {
  refresh(docIndex, version);
}

void Session::refresh(const std::uint32_t docIndex,
                      const MicroversionId &version) {
  if (docIndex >= open.size()) {
    return;
  }
  open[docIndex].version = version;
  open[docIndex].pieces  = docStore.rebuild(version);
}

std::shared_ptr<VersionTextSource>
Session::sourceFor(const MicroversionId &version) const {
  return std::make_shared<VersionTextSource>(docStore.textOf(version), version);
}

void Session::textInserted(Doc &doc, const std::uint32_t at,
                           const std::string &utf8) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  // The library has already spliced the text; this records what it means. The
  // state the operation produces is where this document now is, so typing is
  // what moves it through hypertime.
  const auto produced = docStore.insert(open[which].version, at, utf8);
  refresh(which, produced);
  std::cout << "xudu: " << produced.str() << " insert " << utf8.size()
            << " bytes at " << at << "\n";
}

void Session::textErased(Doc &doc, const std::uint32_t at,
                         const std::string &removed) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  // Rearrange to limbo: the content stays in the primedia spool, so every
  // earlier state still resolves and this is not destructive.
  const auto produced = docStore.erase(
      open[which].version, at, static_cast<std::uint32_t>(removed.size()));
  refresh(which, produced);
  std::cout << "xudu: " << produced.str() << " delete " << removed.size()
            << " bytes at " << at << "\n";
}

void Session::decorate(const Doc &doc,
                       std::vector<gleditor::SpanStyle> &out) {
  const auto which = doc.documentIndex();
  if (which >= open.size()) {
    return;
  }
  const auto &mine = open[which].pieces;

  // Passages this document shares with another open one. Found by comparing
  // primedia addresses, which is why two documents that merely read the same
  // are not reported and a quotation edited around still is.
  for (std::size_t other = 0; other < open.size(); other++) {
    if (other == which) {
      continue;
    }
    for (const auto &piece : open[other].pieces.pieces()) {
      for (const auto &extent : mine.occurrencesOf(piece)) {
        out.push_back(gleditor::SpanStyle{extent.start, extent.end,
                                          Session::transclusionColour});
      }
    }
  }

  // Passages a link is attached to. A link names content, so it shows up here
  // for any document quoting that content, which is the whole of Nelson's
  // "present on all manifestations".
  for (const auto &[id, link] : docStore.links()) {
    for (const auto *const ends : {&link.left, &link.right}) {
      for (const auto &span : *ends) {
        for (const auto &extent : mine.occurrencesOf(span)) {
          out.push_back(
              gleditor::SpanStyle{extent.start, extent.end, Session::linkColour});
        }
      }
    }
  }
}

// -- the hypertime map --------------------------------------------------------

HypertimeMap::HypertimeMap(std::string aFontName, const Session &aSession)
    : fontName(std::move(aFontName)), session(aSession) {}

void HypertimeMap::deviceReady(render::RenderDevice &device,
                               const render::PipelineDesc &documentPipeline) {
  canvas = std::make_unique<gleditor::Canvas>(&device, fontName);
  canvas->createPipeline(documentPipeline);
}

void HypertimeMap::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible || nullptr == canvas) {
    return;
  }

  // Rebuilding means laying out a glyph per character of every label and
  // re-uploading, so it is done when something changed rather than per frame.
  const auto opCount = session.store().opCount();
  if (opCount != builtForOps || current != builtForCurrent ||
      !builtForVisible || ctx.screenHeight != builtForHeight) {
    builtForOps     = opCount;
    builtForCurrent = current;
    builtForVisible = true;
    builtForHeight  = ctx.screenHeight;

    canvas->clear();

    // A column per generation, so time runs left to right; branches off one
    // state stack downwards within their column.
    const auto versions = session.store().allVersions();
    std::map<std::string, std::pair<float, float>> placed;
    std::vector<int> rowsUsed;

    const auto depthOf = [](const MicroversionId &id) {
      return static_cast<int>(id.path().size());
    };

    int widest = 0;
    for (const auto &id : versions) {
      widest = std::max(widest, depthOf(id));
    }
    rowsUsed.assign(static_cast<std::size_t>(widest) + 1, 0);

    const auto top = static_cast<float>(ctx.screenHeight) - mapMargin;

    // The panel behind it all, sized to what will be drawn on it.
    const auto panelWidth =
        (static_cast<float>(widest) * (nodeWidth + columnGap)) + (2 * padding);
    float panelHeight = padding;
    {
      std::vector<int> counts(rowsUsed.size(), 0);
      for (const auto &id : versions) {
        counts[static_cast<std::size_t>(depthOf(id))]++;
      }
      const auto tallest = counts.empty()
                               ? 0
                               : *std::ranges::max_element(counts);
      panelHeight = (static_cast<float>(tallest) * (nodeHeight + rowGap)) +
                    (2 * padding) + nodeHeight;
    }

    canvas->setTag(render::packTagIdentity(render::tagKindOverlay, 0, 0));
    const auto heading = "hypertime: " + std::to_string(versions.size()) +
                         " states, none lost";
    const auto headingWidth = canvas->measureText(heading).width;
    canvas->addRect(mapMargin, top - panelHeight,
                   std::max(panelWidth, headingWidth + (2 * padding)),
                   panelHeight, Doc::VBORow::color3(24, 26, 34));
    canvas->addText(ctx.state, mapMargin + padding, top - padding, heading,
                   Doc::VBORow::color(226), Doc::VBORow::color3(24, 26, 34));

    for (const auto &id : versions) {
      const auto depth  = depthOf(id);
      auto &row         = rowsUsed[static_cast<std::size_t>(depth)];
      const auto left   = mapMargin + padding +
                        (static_cast<float>(depth - 1) * (nodeWidth + columnGap));
      const auto bottom = top - padding - nodeHeight -
                          (static_cast<float>(row + 1) * (nodeHeight + rowGap));
      row++;
      placed.emplace(id.str(), std::make_pair(left, bottom));

      // An edge back to the state this one followed, so a branch reads as a
      // fork rather than as two unrelated columns. Drawn as an elbow rather
      // than a diagonal: the only primitive here is an axis-aligned quad, so a
      // sloped line comes out as its bounding box -- a grey slab between the
      // two nodes. Two segments meeting at a right angle are exact, and are
      // how a graph of this shape is usually drawn anyway.
      if (const auto parent = placed.find(id.parent().str());
          parent != placed.end()) {
        constexpr float edge = 2.0F;
        const auto edgeColour = Doc::VBORow::color3(90, 96, 112);
        const auto fromX      = parent->second.first + nodeWidth;
        const auto fromY      = parent->second.second + (nodeHeight / 2.0F);
        const auto toY        = bottom + (nodeHeight / 2.0F);
        const auto turn       = fromX + (columnGap / 2.0F);
        canvas->addLine(fromX, fromY, turn, fromY, edge, edgeColour);
        canvas->addLine(turn, fromY, turn, toY, edge, edgeColour);
        canvas->addLine(turn, toY, left, toY, edge, edgeColour);
      }

      const bool here = id == current;
      const auto box  = here ? Doc::VBORow::color3(58, 96, 150)
                             : Doc::VBORow::color3(44, 48, 60);
      // Each node carries its own picking identity, so a click on the map can
      // be resolved to the state it landed on.
      canvas->setTag(render::packTagIdentity(render::tagKindOverlay, 0, 0),
                    static_cast<std::uint32_t>(placed.size()));
      canvas->addRect(left, bottom, nodeWidth, nodeHeight, box);
      canvas->addText(ctx.state, left + 6.0F, bottom + nodeHeight - 5.0F,
                     id.str(), Doc::VBORow::color(here ? 255 : 208), box);
    }

    canvas->commit();
  }

  // Window pixels with Y running up, matching what the canvas builds in.
  const glm::mat4 projection =
      glm::ortho(0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
                 static_cast<float>(ctx.screenHeight));
  canvas->draw(ctx.state, projection);
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:

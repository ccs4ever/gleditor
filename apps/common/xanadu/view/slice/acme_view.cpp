#include "common/xanadu/view/slice/acme_view.hpp"
#include <algorithm>
#include <vector>

namespace xanadu::view {

AcmeSliceView::AcmeSliceView(std::optional<zigzag::DimRef> dimLayoutX,
                             std::optional<zigzag::DimRef> dimLayoutY,
                             std::optional<zigzag::DimRef> dimTag,
                             std::optional<zigzag::DimRef> dimBody,
                             std::optional<zigzag::DimRef> dimEvent,
                             float tagHeight, float margin)
    : dimLayoutX_(dimLayoutX), dimLayoutY_(dimLayoutY), dimTag_(dimTag),
      dimBody_(dimBody), dimEvent_(dimEvent), tagHeight_(tagHeight),
      margin_(margin) {}

std::optional<glm::vec3>
AcmeSliceView::axisDirection(ViewAxisId axis) const noexcept {
  switch (axis) {
  case 0:
    return glm::vec3(1.0F, 0.0F, 0.0F); // X: Column horizontal tiling
  case 1:
    return glm::vec3(0.0F, -1.0F, 0.0F); // Y: Pane vertical stacking (downward)
  case 2:
    return glm::vec3(0.0F, 0.0F, 1.0F); // Z: Layering
  default:
    return std::nullopt;
  }
}

void AcmeSliceView::layout(const SliceLayoutInput &in,
                           LayoutSink &out) const noexcept {
  if (!in.cursor.origin.has_value()) {
    return;
  }
  const auto origin = *in.cursor.origin;

  // 1. Gather columns along d.layout_x
  std::vector<zigzag::CellRef> columns;
  {
    zigzag::CellRef curr = origin;
    while (true) {
      columns.push_back(curr);
      if (!dimLayoutX_.has_value()) {
        break;
      }
      auto next =
          in.space.baseLinked(curr, *dimLayoutX_, zigzag::DimVector::POS);
      if (!next.has_value() || *next == curr || *next == origin) {
        break;
      }
      curr = *next;
    }
  }

  if (columns.empty()) {
    return;
  }

  const float totalWidth  = in.frame.widthPx;
  const float totalHeight = in.frame.heightPx;
  const float colWidth =
      (totalWidth - (margin_ * static_cast<float>(columns.size() + 1))) /
      static_cast<float>(columns.size());

  // 2. Layout each column and its panes along d.layout_y
  for (std::size_t colIdx = 0; colIdx < columns.size(); ++colIdx) {
    const float colLeft =
        margin_ + (static_cast<float>(colIdx) * (colWidth + margin_));

    // Gather panes in this column
    std::vector<zigzag::CellRef> panes;
    {
      zigzag::CellRef curr = columns[colIdx];
      while (true) {
        panes.push_back(curr);
        if (!dimLayoutY_.has_value()) {
          break;
        }
        auto next =
            in.space.baseLinked(curr, *dimLayoutY_, zigzag::DimVector::POS);
        if (!next.has_value() || *next == curr || *next == columns[colIdx]) {
          break;
        }
        curr = *next;
      }
    }

    if (panes.empty()) {
      continue;
    }

    const float paneHeight =
        (totalHeight - (margin_ * static_cast<float>(panes.size() + 1))) /
        static_cast<float>(panes.size());

    for (std::size_t paneIdx = 0; paneIdx < panes.size(); ++paneIdx) {
      const auto paneCell = panes[paneIdx];
      const float paneTop =
          margin_ + (static_cast<float>(paneIdx) * (paneHeight + margin_));

      // Emit PlacedFrame for the window/pane
      PlacedFrame pFrame{};
      pFrame.id          = SubjectId{SubjectKind::Cell, 0, paneCell, 0};
      pFrame.centre      = glm::vec3(colLeft + (colWidth * 0.5F),
                                     paneTop + (paneHeight * 0.5F), 0.0F);
      pFrame.width       = colWidth;
      pFrame.height      = paneHeight;
      const auto frameId = out.push(pFrame);

      // Emit Tag line
      const float actualTagHeight = std::min(tagHeight_, paneHeight * 0.4F);
      PlacedFrame tagFrame{};
      tagFrame.id     = SubjectId{SubjectKind::Cell, 1, paneCell, 0};
      tagFrame.centre = glm::vec3(colLeft + (colWidth * 0.5F),
                                  paneTop + (actualTagHeight * 0.5F), 0.0F);
      tagFrame.width  = colWidth;
      tagFrame.height = actualTagHeight;
      tagFrame.parent = frameId;
      out.push(tagFrame);

      // Walk tag cells along d.tag
      if (dimTag_.has_value()) {
        auto tagCell =
            in.space.baseLinked(paneCell, *dimTag_, zigzag::DimVector::POS);
        float tagCellX = colLeft + margin_;
        while (tagCell.has_value()) {
          PlacedItem item{};
          item.id     = SubjectId{SubjectKind::Cell, 2, *tagCell, 0};
          item.centre = glm::vec3(tagCellX + 30.0F,
                                  paneTop + (actualTagHeight * 0.5F), 0.1F);
          item.width  = 60.0F;
          item.height = actualTagHeight - 2.0F;
          item.flags  = ItemFlags::itemTinted;
          out.push(item);

          tagCellX += 65.0F;
          if (tagCellX >= colLeft + colWidth - margin_) {
            break;
          }
          auto nextTag =
              in.space.baseLinked(*tagCell, *dimTag_, zigzag::DimVector::POS);
          if (!nextTag.has_value() || *nextTag == *tagCell) {
            break;
          }
          tagCell = nextTag;
        }
      }

      // Emit Body items along d.body
      if (dimBody_.has_value()) {
        const float bodyTop = paneTop + actualTagHeight + margin_;
        const float bodyHeight =
            paneHeight - actualTagHeight - (margin_ * 2.0F);

        auto bodyCell =
            in.space.baseLinked(paneCell, *dimBody_, zigzag::DimVector::POS);
        float bodyCellY = bodyTop;
        while (bodyCell.has_value()) {
          PlacedItem item{};
          item.id = SubjectId{SubjectKind::Cell, 3, *bodyCell, 0};
          item.centre =
              glm::vec3(colLeft + (colWidth * 0.5F), bodyCellY + 10.0F, 0.0F);
          item.width  = colWidth - (margin_ * 2.0F);
          item.height = 20.0F;
          out.push(item);

          bodyCellY += 22.0F;
          if (bodyCellY >= bodyTop + bodyHeight) {
            break;
          }
          auto nextBody =
              in.space.baseLinked(*bodyCell, *dimBody_, zigzag::DimVector::POS);
          if (!nextBody.has_value() || *nextBody == *bodyCell) {
            break;
          }
          bodyCell = nextBody;
        }
      }
    }
  }
}

AcmeChordResult
AcmeSliceView::dispatchChord(const ViewManifold &space, const AcmeEvent &event,
                             zigzag::CommandMailbox *mailbox,
                             zigzag::ArenaManifold *mailboxArena) const {
  switch (event.chord) {
  case AcmeChord::B1_Select: {
    AcmeChordResult res{};
    res.action = AcmeChordResult::Action::HandledBuiltin;
    res.output = "Selected: " + event.text;
    return res;
  }
  case AcmeChord::B2_Execute: {
    // 1. Check if d.event has an active listener
    if (dimEvent_.has_value() && event.pane.has_value()) {
      auto listener =
          space.baseLinked(*event.pane, *dimEvent_, zigzag::DimVector::POS);
      if (listener.has_value()) {
        AcmeChordResult res{};
        res.action = AcmeChordResult::Action::InterceptedByEventRank;
        res.output = "Event forwarded to d.event listener cell " +
                     std::to_string(*listener);
        return res;
      }
    }

    // 2. Check Acme built-in verbs
    if (event.text == "Cut" || event.text == "Del" || event.text == "Get" ||
        event.text == "Put" || event.text == "Split" || event.text == "Dump") {
      AcmeChordResult res{};
      res.action = AcmeChordResult::Action::HandledBuiltin;
      res.output = "Executed Acme built-in: " + event.text;
      return res;
    }

    // 3. Dispatch to CommandMailbox if provided
    if (mailbox != nullptr && mailboxArena != nullptr &&
        event.pane.has_value()) {
      mailbox->submitTransaction(
          *mailboxArena, *event.pane, event.text, [](std::string_view verb) {
            return zigzag::CommandResult{true, "Executed " + std::string(verb)};
          });
      AcmeChordResult res{};
      res.action = AcmeChordResult::Action::ExecutedCommand;
      res.output = "Submitted to CommandMailbox: " + event.text;
      return res;
    }

    AcmeChordResult res{};
    res.action = AcmeChordResult::Action::Unhandled;
    return res;
  }
  case AcmeChord::B3_Look: {
    // 1. Structural link navigation: check if pane has a link along d.body or
    // other dim
    if (dimBody_.has_value() && event.pane.has_value()) {
      auto target =
          space.baseLinked(*event.pane, *dimBody_, zigzag::DimVector::POS);
      if (target.has_value() && *target != *event.pane) {
        AcmeChordResult res{};
        res.action     = AcmeChordResult::Action::NavigatedLink;
        res.targetCell = *target;
        res.output     = "Navigated to linked cell " + std::to_string(*target);
        return res;
      }
    }

    // 2. Fallback to literal text search
    AcmeChordResult res{};
    res.action = AcmeChordResult::Action::TextSearchFallback;
    res.output = "Searching text: " + event.text;
    return res;
  }
  }

  return AcmeChordResult{};
}

} // namespace xanadu::view

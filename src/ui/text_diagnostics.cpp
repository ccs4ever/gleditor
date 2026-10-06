#include <gleditor/ui/text_diagnostics.hpp>

#include <gleditor/logging.hpp>

namespace gleditor::ui {

bool reportTextOverflow(const TextBounds &run, const TextBounds &parent,
                        const std::uint32_t tagKind,
                        const std::uint32_t tagIndex) {
  if (parent.contains(run)) {
    return false;
  }
  GLEDITOR_LOG_DEBUG(
      "ui.layout",
      "text overflow: tag={}:{} run=({},{},{},{}) parent=({},{},{},{})",
      tagKind, tagIndex, run.left, run.bottom, run.width, run.height,
      parent.left, parent.bottom, parent.width, parent.height);
  return true;
}

} // namespace gleditor::ui

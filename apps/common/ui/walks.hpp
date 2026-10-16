#ifndef COMMON_UI_WALKS_HPP
#define COMMON_UI_WALKS_HPP

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gleditor/form.hpp>

#include "common/xanadu/store_activity_log.hpp"

namespace xanadu {

enum class WalksActionKind { Preview, Enter, Reference, Annotate };
struct WalksAction {
  WalksActionKind kind{WalksActionKind::Preview};
  VisitId visit;
  std::string annotation;
};
using WalksAccepted = std::function<void(WalksAction)>;

/// The snapshot and callback keep this presentation independent of thread and
/// navigation ownership. Current is the reading place; selected is only the
/// form cursor. The host dispatches only Enter to the navigator.
void openWalksForm(gleditor::Form &form, std::vector<WalkEntry> entries,
                   std::optional<VisitId> current,
                   std::optional<VisitId> selected,
                   std::function<std::string(const OccurrenceSite &)> describe,
                   WalksAccepted accepted, std::string notice = {});

} // namespace xanadu

#endif

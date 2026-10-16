#include "common/ui/walks.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace xanadu {

void openWalksForm(gleditor::Form &form, std::vector<WalkEntry> entries,
                   const std::optional<VisitId> current,
                   const std::optional<VisitId> selected,
                   std::function<std::string(const OccurrenceSite &)> describe,
                   WalksAccepted accepted, std::string notice) {
  using Field = gleditor::Form::Field;
  using Kind  = gleditor::Form::Kind;
  Field visit{.label = "Saved visit",
              .hint  = "choose a root or branch",
              .kind  = Kind::Choice};
  struct Pending {
    std::size_t index{}, depth{};
  };
  std::vector<Pending> pending;
  for (std::size_t i = entries.size(); i > 0; --i)
    if (!entries[i - 1].visit.parent) pending.push_back({i - 1, 0});
  std::vector<WalkEntry> ordered;
  ordered.reserve(entries.size());
  while (!pending.empty()) {
    const auto [index, depth] = pending.back();
    pending.pop_back();
    const auto &entry   = entries[index];
    std::string caption = depth == 0 ? "Root " : "Branch ";
    caption += "visit " + std::to_string(entry.visit.id.value);
    if (entry.visit.parent)
      caption += " from " + std::to_string(entry.visit.parent->value);
    if (current == entry.visit.id) caption += " (current)";
    if (entry.referenced) caption += " (referenced)";
    caption += ": " + describe(entry.visit.target);
    visit.options.push_back(std::move(caption));
    std::string detail = describe(entry.visit.target);
    if (current == entry.visit.id) detail += "; current reading place";
    if (entry.referenced) detail += "; saved reference";
    for (const auto &annotation : entry.annotations)
      detail += "; Annotation: " + annotation;
    visit.optionDescriptions.push_back(std::move(detail));
    if (selected == entry.visit.id) visit.chosen = ordered.size();
    ordered.push_back(entry);
    for (std::size_t i = entries.size(); i > 0; --i)
      if (entries[i - 1].visit.parent == entry.visit.id)
        pending.push_back({i - 1, depth + 1});
  }
  if (notice.empty())
    notice = ordered.empty()
                 ? "No saved visits yet. Close Walks to continue reading."
                 : "Preview keeps your reading place. Enter restores a saved "
                   "visit. References and annotations stay in your activity.";
  notice = (current ? "Current visit " + std::to_string(current->value)
                    : "No current visit") +
           ". " + notice;
  if (!ordered.empty()) {
    const auto &picked = ordered[visit.chosen];
    notice += " Selected visit " + std::to_string(picked.visit.id.value) +
              ": " + describe(picked.visit.target);
    if (picked.referenced) notice += " (referenced)";
    for (const auto &annotation : picked.annotations)
      notice += " Annotation: " + annotation;
  }
  Field action{.label   = "Action",
               .hint    = "choose what to do with this visit",
               .kind    = Kind::Choice,
               .options = {"Preview", "Enter", "Reference", "Annotate"}};
  Field annotation{.label = "Annotation", .hint = "note for the chosen visit"};
  form.open("Walks", std::move(notice),
            {std::move(visit), std::move(action), std::move(annotation)},
            [entries = std::move(ordered), accepted = std::move(accepted)](
                const std::vector<Field> &answers) {
              constexpr std::array kinds{
                  WalksActionKind::Preview, WalksActionKind::Enter,
                  WalksActionKind::Reference, WalksActionKind::Annotate};
              if (answers.size() != 3 || answers[0].chosen >= entries.size() ||
                  answers[1].chosen >= kinds.size())
                return;
              accepted({.kind       = kinds[answers[1].chosen],
                        .visit      = entries[answers[0].chosen].visit.id,
                        .annotation = answers[2].value});
            });
}

} // namespace xanadu

/**
 * @file link_navigation.hpp
 * @brief One reader's navigation of one selected link, and the commands that
 *        drive it.
 *
 * The selected-link context of design/ui/prototypes/link-context.md: a link
 * pinned whole, an active side, and an independent member and occurrence
 * cursor per side. Every input -- a beam or text pick, a sovereign keymap
 * action, an accessibility action -- becomes one NavigationCommand, and
 * LinkNavigator::dispatch() is the only thing that interprets it, so the three
 * cannot disagree about what a gesture means.
 *
 * Nothing here draws, moves a caret or reads a store. A command answers with
 * a NavigationEffect for the host to carry out; only Enter, Return to origin
 * and Activity Back ever carry a focus target, and only Enter and
 * recordArrival() append a visit.
 */
#ifndef COMMON_XANADU_LINK_NAVIGATION_HPP
#define COMMON_XANADU_LINK_NAVIGATION_HPP

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/cpp26.hpp>

#include "common/xanadu/link_occurrences.hpp"

namespace xanadu {

/// A visit's identity within the reader's activity record.
struct VisitId {
  std::uint64_t value{};

  auto operator<=>(const VisitId &) const = default;
};

/// One side's choices. Either may be unset, and an unset one stays unset
/// rather than defaulting to the first entry.
struct SideCursor {
  std::optional<std::uint32_t> member;
  std::optional<std::uint32_t> occurrence{};

  bool operator==(const SideCursor &) const = default;
};

/// The link state a visit saves, so Activity Back can put the panel back.
struct LinkVisitContext {
  LinkKey key;
  LinkSide active{LinkSide::Left};
  SideCursor left;
  SideCursor right;
  /// Where the reader was when the link was selected.
  std::optional<VisitId> origin;

  bool operator==(const LinkVisitContext &) const = default;
};

/// How the reader arrived at a visit.
enum class Arrival : std::uint8_t {
  /// Opened or jumped to by something other than a link.
  Opened,
  /// Entered an endpoint of the selected link.
  EnteredEndpoint,
};

/// One completed semantic focus transition.
struct Visit {
  VisitId id{};
  std::optional<VisitId> parent;
  OccurrenceSite target;
  Arrival arrival{Arrival::Opened};
  std::optional<LinkVisitContext> link{};

  bool operator==(const Visit &) const = default;
};

/**
 * @brief Where completed visits are kept.
 *
 * The interface the system://activity store will implement; until then
 * InMemoryActivityLog holds a session's visits and forgets them at exit.
 */
class ActivityLog {
public:
  ActivityLog()                               = default;
  ActivityLog(const ActivityLog &)            = delete;
  ActivityLog &operator=(const ActivityLog &) = delete;
  ActivityLog(ActivityLog &&)                 = delete;
  ActivityLog &operator=(ActivityLog &&)      = delete;
  virtual ~ActivityLog()                      = default;

  /// File @p visit, whose id is ignored, and answer the id it now has.
  virtual VisitId append(Visit visit) = 0;

  [[nodiscard]] virtual gleditor::cpp26::optional<const Visit &>
  find(VisitId id) const = 0;
};

class InMemoryActivityLog final : public ActivityLog {
public:
  VisitId append(Visit visit) override;
  [[nodiscard]] gleditor::cpp26::optional<const Visit &>
  find(VisitId id) const override;

  [[nodiscard]] std::size_t size() const noexcept { return visits.size(); }

private:
  std::vector<Visit> visits;
};

/**
 * @brief What a selecting gesture knew about where it landed.
 *
 * A pick on marked text knows the site it hit; an accessibility member node
 * knows the side and member; a beam body knows neither. The hint is settled
 * once the link's occurrences arrive, and a site matching more than one
 * member settles on none rather than guessing.
 */
struct SelectionHint {
  std::optional<OccurrenceSite> hit{};
  std::optional<LinkSide> side{};
  std::optional<std::uint32_t> member{};
  std::optional<std::uint32_t> occurrence{};

  bool operator==(const SelectionHint &) const = default;
};

namespace nav {

struct SelectLink {
  LinkKey key;
  SelectionHint hint{};
  bool operator==(const SelectLink &) const = default;
};
/// Select the next (+1) or previous (-1) link among the host's candidates.
struct StepLink {
  int delta{1};
  bool operator==(const StepLink &) const = default;
};
struct SelectMember {
  LinkKey key;
  LinkSide side{LinkSide::Left};
  std::uint32_t member{};
  bool operator==(const SelectMember &) const = default;
};
/// Step the active side's member cursor.
struct StepMember {
  int delta{1};
  bool operator==(const StepMember &) const = default;
};
struct SelectOccurrence {
  LinkKey key;
  LinkSide side{LinkSide::Left};
  std::uint32_t member{};
  std::uint32_t occurrence{};
  bool operator==(const SelectOccurrence &) const = default;
};
/// Step the active side's occurrence cursor within its chosen member.
struct StepOccurrence {
  int delta{1};
  bool operator==(const StepOccurrence &) const = default;
};
struct Cross {
  bool operator==(const Cross &) const = default;
};
/// Enter the active side's chosen occurrence.
struct Enter {
  bool operator==(const Enter &) const = default;
};
/// Choose an occurrence and enter it in one gesture.
struct EnterAt {
  LinkKey key;
  LinkSide side{LinkSide::Left};
  std::uint32_t member{};
  std::uint32_t occurrence{};
  bool operator==(const EnterAt &) const = default;
};
struct ActivityBack {
  bool operator==(const ActivityBack &) const = default;
};
struct ReturnToOrigin {
  bool operator==(const ReturnToOrigin &) const = default;
};
struct Dismiss {
  bool operator==(const Dismiss &) const = default;
};

} // namespace nav

using NavigationCommand =
    std::variant<nav::SelectLink, nav::StepLink, nav::SelectMember,
                 nav::StepMember, nav::SelectOccurrence, nav::StepOccurrence,
                 nav::Cross, nav::Enter, nav::EnterAt, nav::ActivityBack,
                 nav::ReturnToOrigin, nav::Dismiss>;

enum class NavigationError : std::uint8_t {
  NoLinkSelected,
  /// The selected link's occurrences have not been supplied yet.
  LinkPending,
  MemberOutOfRange,
  OccurrenceOutOfRange,
  NoMemberChosen,
  NoOccurrenceChosen,
  /// The chosen member has no occurrence in the views resolved against.
  MemberNotInView,
  /// Occurrences supplied for a selection that has since been replaced.
  StaleGeneration,
  LinkNotFound,
  NoOrigin,
  NoPreviousVisit,
  NoCandidates,
};

/// Ask the host to resolve @p key and hand the result to supply().
struct ResolveRequest {
  LinkKey key;
  std::uint64_t generation{};
  bool operator==(const ResolveRequest &) const = default;
};

/// A member, and perhaps one of its occurrences, to show without moving.
struct Preview {
  LinkSide side{LinkSide::Left};
  std::uint32_t member{};
  std::optional<std::uint32_t> occurrence;
  bool operator==(const Preview &) const = default;
};

/// What the host must do after a command. Empty means nothing moves.
struct NavigationEffect {
  std::optional<ResolveRequest> resolve{};
  std::optional<Preview> preview{};
  std::optional<OccurrenceSite> focus{};
  /// The visit the reader is now at, when that changed.
  std::optional<VisitId> visit{};
  /// The link context was removed.
  bool dismissed{false};

  bool operator==(const NavigationEffect &) const = default;
};

/// The pinned link, whole.
struct SelectedLink {
  LinkKey key;
  std::uint64_t generation{};
  /// Unset until supply() delivers them.
  std::optional<LinkOccurrences> occurrences{};
  LinkSide active{LinkSide::Left};
  SideCursor left{};
  SideCursor right{};
  /// Where the reader was when the link was selected.
  std::optional<VisitId> origin;
  /// Settled against the occurrences when they arrive.
  SelectionHint hint{};

  [[nodiscard]] const SideCursor &cursor(const LinkSide side) const noexcept {
    return LinkSide::Left == side ? left : right;
  }
  [[nodiscard]] SideCursor &cursor(const LinkSide side) noexcept {
    return LinkSide::Left == side ? left : right;
  }
};

using NavigationResult = std::expected<NavigationEffect, NavigationError>;

class LinkNavigator {
public:
  explicit LinkNavigator(ActivityLog &activity) noexcept : activity(activity) {}

  NavigationResult dispatch(const NavigationCommand &command);

  /**
   * @brief Deliver the occurrences a ResolveRequest asked for.
   *
   * Refused with StaleGeneration unless @p generation is the current
   * selection's, so a slow resolution cannot land on a link the reader has
   * since left. Cursors already set are kept when still in range.
   */
  NavigationResult
  supply(std::uint64_t generation,
         std::expected<LinkOccurrences, LinkQueryError> occurrences);

  /// Record arriving at @p site by some means other than a link.
  VisitId recordArrival(const OccurrenceSite &site);

  /// The links StepLink walks, in the host's stable order.
  void setCandidates(std::vector<LinkKey> keys) {
    candidates = std::move(keys);
  }

  [[nodiscard]] gleditor::cpp26::optional<const SelectedLink &>
  selection() const noexcept;

  [[nodiscard]] std::optional<VisitId> currentVisit() const noexcept {
    return current;
  }

private:
  NavigationResult select(const LinkKey &key, SelectionHint hint);
  NavigationResult stepLink(int delta);
  NavigationResult selectMember(const nav::SelectMember &command);
  NavigationResult stepMember(int delta);
  NavigationResult selectOccurrence(const nav::SelectOccurrence &command);
  NavigationResult stepOccurrence(int delta);
  NavigationResult cross();
  NavigationResult enter();
  NavigationResult enterAt(const nav::EnterAt &command);
  NavigationResult activityBack();
  NavigationResult returnToOrigin();
  NavigationResult dismiss();

  /// The selection, once its occurrences have been supplied.
  std::expected<SelectedLink *, NavigationError> resolved();
  NavigationResult restoreVisit(const Visit &visit);

  ActivityLog &activity;
  std::optional<SelectedLink> selected;
  std::optional<VisitId> current;
  std::vector<LinkKey> candidates;
  std::uint64_t generations{};
};

// -- input adapters --------------------------------------------------------
//
// Each input surface maps to commands here rather than in its own code, so a
// test can drive all three through one scenario and compare the outcomes.

/// A pointer pick on link @p key, at @p hit when it landed on content.
[[nodiscard]] NavigationCommand
commandForPick(const LinkKey &key, std::optional<OccurrenceSite> hit);

/// The command a sovereign keymap action names, if it names one.
[[nodiscard]] std::optional<NavigationCommand>
commandForAction(std::string_view action);

namespace a11y_node {
struct Link {
  LinkKey key;
};
struct Member {
  LinkKey key;
  LinkSide side{LinkSide::Left};
  std::uint32_t member{};
};
struct Occurrence {
  LinkKey key;
  LinkSide side{LinkSide::Left};
  std::uint32_t member{};
  std::uint32_t occurrence{};
};
} // namespace a11y_node

/// What an accessibility node stands for. The host allocates node ids.
using AccessibleLinkNode =
    std::variant<a11y_node::Link, a11y_node::Member, a11y_node::Occurrence>;

/**
 * @brief The command an accessibility action on @p node asks for.
 *
 * Focus previews without moving anything; Click is the node's primary
 * gesture -- select the link, choose the member, enter the occurrence.
 */
[[nodiscard]] std::optional<NavigationCommand>
commandForAccessibility(const AccessibleLinkNode &node,
                        gleditor::a11y::Action action);

} // namespace xanadu

#endif // COMMON_XANADU_LINK_NAVIGATION_HPP

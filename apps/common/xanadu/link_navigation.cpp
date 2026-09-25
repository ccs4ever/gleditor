/**
 * @file link_navigation.cpp
 * @brief The selected-link command boundary.
 */
#include "common/xanadu/link_navigation.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

#include <gleditor/logging.hpp>
#include <gleditor/ranges.hpp>

#include "common/xanadu/system_docs.hpp"

namespace xanadu {

VisitId InMemoryActivityLog::append(Visit visit) {
  // Numbered from one so that a default VisitId never names a real visit.
  visit.id = VisitId{.value = visits.size() + 1};
  visits.push_back(std::move(visit));
  return visits.back().id;
}

gleditor::cpp26::optional<const Visit &>
InMemoryActivityLog::find(const VisitId id) const {
  if (0 == id.value || id.value > visits.size()) {
    return gleditor::cpp26::nullopt;
  }
  return visits[id.value - 1];
}

namespace {

bool overlaps(const Extent &a, const Extent &b) noexcept {
  return a.start < b.end && b.start < a.end;
}

/// Whether @p hit lands inside @p site: same store, state and cell, and
/// overlapping bytes.
bool lands(const OccurrenceSite &hit, const OccurrenceSite &site) {
  return std::visit(
      [&]<typename Site>(const Site &at) {
        const auto *const other = std::get_if<Site>(&site);
        if (nullptr == other || at.store != other->store ||
            at.version != other->version) {
          return false;
        }
        if constexpr (std::is_same_v<Site, CellSite>) {
          if (at.cell != other->cell) {
            return false;
          }
        }
        return overlaps(at.range, other->range);
      },
      hit);
}

/// Step @p at by @p delta through @p count entries, cycling; an unset cursor
/// steps onto the first entry going forward and the last going back.
std::uint32_t stepIndex(const std::optional<std::uint32_t> at, const int delta,
                        const std::uint32_t count) {
  if (!at) {
    return delta >= 0 ? 0 : count - 1;
  }
  const auto size = static_cast<std::int64_t>(count);
  const auto next = (static_cast<std::int64_t>(*at) + delta) % size;
  return static_cast<std::uint32_t>(next < 0 ? next + size : next);
}

const std::vector<LinkMember> &membersOf(const SelectedLink &link,
                                         const LinkSide side) {
  // Only reached once supply() has filled them; value() makes that a checked
  // precondition rather than undefined behaviour.
  return link.occurrences.value().members(side);
}

/// A member with one occurrence has that occurrence preselected for preview;
/// with several, the reader must choose. Enter stays explicit either way.
SideCursor chooseMember(const LinkMember &member, const std::uint32_t index) {
  return SideCursor{.member     = index,
                    .occurrence = 1 == member.occurrences.size()
                                      ? std::optional<std::uint32_t>{0}
                                      : std::nullopt};
}

std::optional<Preview> previewOf(const SelectedLink &link,
                                 const LinkSide side) {
  const auto &cursor = link.cursor(side);
  if (!cursor.member) {
    return std::nullopt;
  }
  return Preview{
      .side = side, .member = *cursor.member, .occurrence = cursor.occurrence};
}

/// Drop any cursor a revised link no longer has room for.
void keepInRange(SelectedLink &link, const LinkSide side) {
  auto &cursor        = link.cursor(side);
  const auto &members = membersOf(link, side);
  if (cursor.member && *cursor.member >= members.size()) {
    cursor = {};
  } else if (cursor.member && cursor.occurrence &&
             *cursor.occurrence >= members[*cursor.member].occurrences.size()) {
    cursor.occurrence.reset();
  }
}

/// Settle what the selecting gesture knew against the resolved endsets.
void settleHint(SelectedLink &link) {
  const auto hint = std::exchange(link.hint, {});
  if (hint.side) {
    link.active = *hint.side;
  }
  if (hint.side && hint.member) {
    const auto &members = membersOf(link, *hint.side);
    if (*hint.member < members.size()) {
      auto cursor = chooseMember(members[*hint.member], *hint.member);
      if (hint.occurrence &&
          *hint.occurrence < members[*hint.member].occurrences.size()) {
        cursor.occurrence = hint.occurrence;
      }
      link.cursor(*hint.side) = cursor;
    }
    return;
  }
  if (!hint.hit) {
    return;
  }

  struct Landing {
    LinkSide side;
    std::uint32_t member;
    std::uint32_t occurrence;
  };
  std::vector<Landing> landings;
  for (const auto side : {LinkSide::Left, LinkSide::Right}) {
    for (const auto &member : membersOf(link, side)) {
      for (std::uint32_t i = 0; i < member.occurrences.size(); ++i) {
        if (lands(*hint.hit, member.occurrences[i].site)) {
          landings.push_back(
              {.side = side, .member = member.index, .occurrence = i});
        }
      }
    }
  }
  const auto sameMember = [&](const Landing &l) {
    return l.side == landings.front().side &&
           l.member == landings.front().member;
  };
  if (landings.empty() || !std::ranges::all_of(landings, sameMember)) {
    // Overlapping members both claim the spot. Guessing one would hand the
    // reader a pairing nobody authored; leaving both unset makes them choose.
    GLEDITOR_LOG_DEBUG("xudu.links", "pick on link {} settles on no member",
                       link.key.id);
    return;
  }
  const auto &hitAt       = landings.front();
  link.active             = hitAt.side;
  link.cursor(hitAt.side) = SideCursor{
      .member     = hitAt.member,
      .occurrence = 1 == landings.size() ? std::optional{hitAt.occurrence}
                                         : std::nullopt};
}

} // namespace

gleditor::cpp26::optional<const SelectedLink &>
LinkNavigator::selection() const noexcept {
  return gleditor::refOf(selected ? &*selected : nullptr);
}

NavigationResult LinkNavigator::dispatch(const NavigationCommand &command) {
  return std::visit(
      [this]<typename Command>(const Command &c) -> NavigationResult {
        if constexpr (std::is_same_v<Command, nav::SelectLink>) {
          return select(c.key, c.hint);
        } else if constexpr (std::is_same_v<Command, nav::StepLink>) {
          return stepLink(c.delta);
        } else if constexpr (std::is_same_v<Command, nav::SelectMember>) {
          return selectMember(c);
        } else if constexpr (std::is_same_v<Command, nav::StepMember>) {
          return stepMember(c.delta);
        } else if constexpr (std::is_same_v<Command, nav::SelectOccurrence>) {
          return selectOccurrence(c);
        } else if constexpr (std::is_same_v<Command, nav::StepOccurrence>) {
          return stepOccurrence(c.delta);
        } else if constexpr (std::is_same_v<Command, nav::Cross>) {
          return cross();
        } else if constexpr (std::is_same_v<Command, nav::Enter>) {
          return enter();
        } else if constexpr (std::is_same_v<Command, nav::EnterAt>) {
          return enterAt(c);
        } else if constexpr (std::is_same_v<Command, nav::ActivityBack>) {
          return activityBack();
        } else if constexpr (std::is_same_v<Command, nav::ReturnToOrigin>) {
          return returnToOrigin();
        } else {
          static_assert(std::is_same_v<Command, nav::Dismiss>);
          return dismiss();
        }
      },
      command);
}

std::expected<SelectedLink *, NavigationError> LinkNavigator::resolved() {
  if (!selected) {
    return std::unexpected(NavigationError::NoLinkSelected);
  }
  if (!selected->occurrences) {
    return std::unexpected(NavigationError::LinkPending);
  }
  return &*selected;
}

NavigationResult LinkNavigator::select(const LinkKey &key, SelectionHint hint) {
  if (selected && key == selected->key) {
    // Picking the link already pinned keeps what the reader chose on it; the
    // new gesture only adds what it knew.
    selected->hint = std::move(hint);
    if (!selected->occurrences) {
      return NavigationEffect{};
    }
    settleHint(*selected);
    return NavigationEffect{.preview = previewOf(*selected, selected->active)};
  }
  selected = SelectedLink{.key        = key,
                          .generation = ++generations,
                          .active     = hint.side.value_or(LinkSide::Left),
                          .origin     = current,
                          .hint       = std::move(hint)};
  return NavigationEffect{.resolve = ResolveRequest{
                              .key = key, .generation = selected->generation}};
}

NavigationResult LinkNavigator::supply(
    const std::uint64_t generation,
    std::expected<LinkOccurrences, LinkQueryError> occurrences) {
  if (!selected || generation != selected->generation) {
    return std::unexpected(NavigationError::StaleGeneration);
  }
  if (!occurrences) {
    selected.reset();
    return std::unexpected(NavigationError::LinkNotFound);
  }
  if (occurrences->key != selected->key) {
    return std::unexpected(NavigationError::StaleGeneration);
  }
  selected->occurrences = std::move(*occurrences);
  keepInRange(*selected, LinkSide::Left);
  keepInRange(*selected, LinkSide::Right);
  settleHint(*selected);
  return NavigationEffect{.preview = previewOf(*selected, selected->active)};
}

VisitId LinkNavigator::recordArrival(const OccurrenceSite &site) {
  current = activity.append(
      Visit{.parent = current, .target = site, .arrival = Arrival::Opened});
  return *current;
}

NavigationResult LinkNavigator::stepLink(const int delta) {
  if (candidates.empty()) {
    return std::unexpected(NavigationError::NoCandidates);
  }
  std::optional<std::uint32_t> at;
  if (selected) {
    if (const auto found = std::ranges::find(candidates, selected->key);
        candidates.end() != found) {
      at = static_cast<std::uint32_t>(found - candidates.begin());
    }
  }
  const auto next =
      stepIndex(at, delta, static_cast<std::uint32_t>(candidates.size()));
  return select(candidates[next], {});
}

NavigationResult LinkNavigator::selectMember(const nav::SelectMember &command) {
  if (!selected || command.key != selected->key || !selected->occurrences) {
    return select(command.key,
                  {.side = command.side, .member = command.member});
  }
  const auto &members = membersOf(*selected, command.side);
  if (command.member >= members.size()) {
    return std::unexpected(NavigationError::MemberOutOfRange);
  }
  selected->active = command.side;
  selected->cursor(command.side) =
      chooseMember(members[command.member], command.member);
  return NavigationEffect{.preview = previewOf(*selected, command.side)};
}

NavigationResult LinkNavigator::stepMember(const int delta) {
  const auto link = resolved();
  if (!link) {
    return std::unexpected(link.error());
  }
  auto &selection     = **link;
  const auto &members = membersOf(selection, selection.active);
  if (members.empty()) {
    return std::unexpected(NavigationError::MemberOutOfRange);
  }
  const auto next = stepIndex(selection.cursor(selection.active).member, delta,
                              static_cast<std::uint32_t>(members.size()));
  selection.cursor(selection.active) = chooseMember(members[next], next);
  return NavigationEffect{.preview = previewOf(selection, selection.active)};
}

NavigationResult
LinkNavigator::selectOccurrence(const nav::SelectOccurrence &command) {
  if (!selected || command.key != selected->key || !selected->occurrences) {
    return select(command.key, {.side       = command.side,
                                .member     = command.member,
                                .occurrence = command.occurrence});
  }
  const auto &members = membersOf(*selected, command.side);
  if (command.member >= members.size()) {
    return std::unexpected(NavigationError::MemberOutOfRange);
  }
  if (command.occurrence >= members[command.member].occurrences.size()) {
    return std::unexpected(NavigationError::OccurrenceOutOfRange);
  }
  selected->active = command.side;
  selected->cursor(command.side) =
      SideCursor{.member = command.member, .occurrence = command.occurrence};
  return NavigationEffect{.preview = previewOf(*selected, command.side)};
}

NavigationResult LinkNavigator::stepOccurrence(const int delta) {
  const auto link = resolved();
  if (!link) {
    return std::unexpected(link.error());
  }
  auto &selection = **link;
  auto &cursor    = selection.cursor(selection.active);
  if (!cursor.member) {
    return std::unexpected(NavigationError::NoMemberChosen);
  }
  const auto &member = membersOf(selection, selection.active)[*cursor.member];
  if (member.occurrences.empty()) {
    return std::unexpected(NavigationError::MemberNotInView);
  }
  cursor.occurrence =
      stepIndex(cursor.occurrence, delta,
                static_cast<std::uint32_t>(member.occurrences.size()));
  return NavigationEffect{.preview = previewOf(selection, selection.active)};
}

NavigationResult LinkNavigator::cross() {
  const auto link = resolved();
  if (!link) {
    return std::unexpected(link.error());
  }
  auto &selection  = **link;
  selection.active = opposite(selection.active);
  return NavigationEffect{.preview = previewOf(selection, selection.active)};
}

NavigationResult LinkNavigator::enter() {
  const auto link = resolved();
  if (!link) {
    return std::unexpected(link.error());
  }
  const auto &selection = **link;
  const auto &cursor    = selection.cursor(selection.active);
  if (!cursor.member) {
    return std::unexpected(NavigationError::NoMemberChosen);
  }
  const auto &member = membersOf(selection, selection.active)[*cursor.member];
  if (member.occurrences.empty()) {
    return std::unexpected(NavigationError::MemberNotInView);
  }
  if (!cursor.occurrence) {
    return std::unexpected(NavigationError::NoOccurrenceChosen);
  }
  const auto &target = member.occurrences[*cursor.occurrence].site;
  current            = activity.append(
      Visit{.parent  = current,
            .target  = target,
            .arrival = Arrival::EnteredEndpoint,
            .link    = LinkVisitContext{.key    = selection.key,
                                        .active = selection.active,
                                        .left   = selection.left,
                                        .right  = selection.right,
                                        .origin = selection.origin}});
  return NavigationEffect{.focus = target, .visit = current};
}

NavigationResult LinkNavigator::enterAt(const nav::EnterAt &command) {
  const bool ready =
      selected && command.key == selected->key && selected->occurrences;
  const auto chosen = selectOccurrence({.key        = command.key,
                                        .side       = command.side,
                                        .member     = command.member,
                                        .occurrence = command.occurrence});
  // An unresolved link is only selected here: entry waits for the reader to
  // ask again once the occurrence exists, so nothing is entered on a guess.
  if (!chosen || !ready) {
    return chosen;
  }
  return enter();
}

NavigationResult LinkNavigator::restoreVisit(const Visit &visit) {
  current = visit.id;
  NavigationEffect effect{.focus = visit.target, .visit = visit.id};
  if (!visit.link) {
    // A visit made without a link keeps whatever link is pinned now: the
    // reader stepping back to where a trip began still wants the link that
    // made the trip.
    return effect;
  }
  const auto &saved = *visit.link;
  if (selected && saved.key == selected->key && selected->occurrences) {
    selected->active = saved.active;
    selected->left   = saved.left;
    selected->right  = saved.right;
    selected->origin = saved.origin;
    keepInRange(*selected, LinkSide::Left);
    keepInRange(*selected, LinkSide::Right);
    effect.preview = previewOf(*selected, selected->active);
    return effect;
  }
  selected = SelectedLink{.key        = saved.key,
                          .generation = ++generations,
                          .active     = saved.active,
                          .left       = saved.left,
                          .right      = saved.right,
                          .origin     = saved.origin};
  effect.resolve =
      ResolveRequest{.key = saved.key, .generation = selected->generation};
  return effect;
}

NavigationResult LinkNavigator::activityBack() {
  if (!current) {
    return std::unexpected(NavigationError::NoPreviousVisit);
  }
  const auto here = activity.find(*current);
  if (!here || !here->parent) {
    return std::unexpected(NavigationError::NoPreviousVisit);
  }
  const auto parent = activity.find(*here->parent);
  if (!parent) {
    return std::unexpected(NavigationError::NoPreviousVisit);
  }
  return restoreVisit(*parent);
}

NavigationResult LinkNavigator::returnToOrigin() {
  if (!selected) {
    return std::unexpected(NavigationError::NoLinkSelected);
  }
  if (!selected->origin) {
    return std::unexpected(NavigationError::NoOrigin);
  }
  const auto origin = activity.find(*selected->origin);
  if (!origin) {
    return std::unexpected(NavigationError::NoOrigin);
  }
  current = origin->id;
  return NavigationEffect{.focus = origin->target, .visit = current};
}

NavigationResult LinkNavigator::dismiss() {
  const bool had = selected.has_value();
  selected.reset();
  return NavigationEffect{.dismissed = had};
}

std::string_view name(const NavigationError error) noexcept {
  switch (error) {
  case NavigationError::NoLinkSelected:
    return "no link selected";
  case NavigationError::LinkPending:
    return "link still resolving";
  case NavigationError::MemberOutOfRange:
    return "no such member";
  case NavigationError::OccurrenceOutOfRange:
    return "no such occurrence";
  case NavigationError::NoMemberChosen:
    return "no member chosen";
  case NavigationError::NoOccurrenceChosen:
    return "no occurrence chosen";
  case NavigationError::MemberNotInView:
    return "member not in view";
  case NavigationError::StaleGeneration:
    return "stale resolution";
  case NavigationError::LinkNotFound:
    return "link not found";
  case NavigationError::NoOrigin:
    return "no origin";
  case NavigationError::NoPreviousVisit:
    return "no previous visit";
  case NavigationError::NoCandidates:
    return "no links on screen";
  }
  return "unknown";
}

std::string_view name(const NavigationCommand &command) noexcept {
  static constexpr std::string_view names[] = {"select link",
                                               "step link",
                                               "select member",
                                               "step member",
                                               "select occurrence",
                                               "step occurrence",
                                               "cross",
                                               "enter",
                                               "enter at",
                                               "activity back",
                                               "return to origin",
                                               "dismiss"};
  static_assert(std::size(names) == std::variant_size_v<NavigationCommand>);
  return names[command.index()];
}

NavigationCommand commandForPick(const LinkKey &key,
                                 std::optional<OccurrenceSite> hit) {
  return nav::SelectLink{.key = key, .hint = {.hit = std::move(hit)}};
}

std::optional<NavigationCommand>
commandForAction(const std::string_view action) {
  using namespace settings;
  if (kKeymapLinkNext == action) {
    return nav::StepLink{.delta = 1};
  }
  if (kKeymapLinkPrevious == action) {
    return nav::StepLink{.delta = -1};
  }
  if (kKeymapLinkMemberNext == action) {
    return nav::StepMember{.delta = 1};
  }
  if (kKeymapLinkMemberPrevious == action) {
    return nav::StepMember{.delta = -1};
  }
  if (kKeymapLinkOccurrenceNext == action) {
    return nav::StepOccurrence{.delta = 1};
  }
  if (kKeymapLinkOccurrencePrevious == action) {
    return nav::StepOccurrence{.delta = -1};
  }
  if (kKeymapLinkCross == action) {
    return nav::Cross{};
  }
  if (kKeymapLinkEnter == action) {
    return nav::Enter{};
  }
  if (kKeymapLinkOrigin == action) {
    return nav::ReturnToOrigin{};
  }
  if (kKeymapLinkDismiss == action) {
    return nav::Dismiss{};
  }
  if (kKeymapActivityBack == action) {
    return nav::ActivityBack{};
  }
  return std::nullopt;
}

std::optional<NavigationCommand>
commandForAccessibility(const AccessibleLinkNode &node,
                        const gleditor::a11y::Action action) {
  using gleditor::a11y::Action;
  return std::visit(
      [action]<typename Node>(
          const Node &at) -> std::optional<NavigationCommand> {
        if constexpr (std::is_same_v<Node, a11y_node::Link>) {
          // Focusing the link node reads it; selecting is the click.
          if (Action::Click == action) {
            return nav::SelectLink{.key = at.key};
          }
        } else if constexpr (std::is_same_v<Node, a11y_node::Member>) {
          if (Action::Click == action || Action::Focus == action) {
            return nav::SelectMember{
                .key = at.key, .side = at.side, .member = at.member};
          }
        } else {
          if (Action::Focus == action) {
            return nav::SelectOccurrence{.key        = at.key,
                                         .side       = at.side,
                                         .member     = at.member,
                                         .occurrence = at.occurrence};
          }
          if (Action::Click == action) {
            return nav::EnterAt{.key        = at.key,
                                .side       = at.side,
                                .member     = at.member,
                                .occurrence = at.occurrence};
          }
        }
        return std::nullopt;
      },
      node);
}

} // namespace xanadu

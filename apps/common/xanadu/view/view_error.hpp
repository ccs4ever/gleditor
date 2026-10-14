/**
 * @file view_error.hpp
 * @brief Why a view-space call refused, and what the reader is told.
 *
 * design/view-system.md §8.11 and §12.2. A refusal and a message are two
 * enumerations because they are two different sets: several messages report
 * something that is not an error at all (nothing further that way, a toss
 * that discarded view cells, a saved binding whose dimension is gone), and
 * the presenter, the raster and the accessibility announcement must all find
 * a refusal's words the same way. messageKey() is the one bridge from the
 * first to the second.
 *
 * The words are templates. A field in braces is filled by whoever raises the
 * message, which knows the names; nothing here formats or looks anything up.
 */
#ifndef COMMON_XANADU_VIEW_VIEW_ERROR_HPP
#define COMMON_XANADU_VIEW_VIEW_ERROR_HPP

#include <array>
#include <cstdint>
#include <string_view>

namespace xanadu::view {

enum class ViewError : std::uint8_t {
  OccupiedDirection,  // link(): an end already has a neighbour there
  RealCellInViewLink, // link(): an end or the key is not a view cell of that
                      // arena
  StaleEpoch,         // a ViewCellRef from a tossed generation
  UnknownTarget,      // an occurrence's target is neither real nor a live group
  UnknownAxis,
  GroupCycle,
  EmptyGroupBind,
  DuplicateViewKind,
  ChordCollision,
  PromotionRefused, // promote()'s own budget or refusal
  ArenaRefused,     // the arena refused; carries nothing more
  UnknownPlace,     // a member, ring or list position past the end
  UnknownName,      // a typed name that is neither a dimension nor a group
  BadCommand,       // a typed command that does not say a point and a target
  BadPointName,     // a binding point with no name, or one already taken
};

/// Everything a view tells the reader, in a toast, the raster or an
/// announcement (§12.2).
enum class ViewMessage : std::uint8_t {
  NothingThatWay,
  NoPackThatWay,
  RebindDiscarded,
  EmptyGroup,
  GroupInsideItself,
  SavedBindingGone,
  GroupRemovedFromUse,
  PackTooLargeToKeep,
  PagesStillArriving,
  DirectionOccupied,
  RealCellRefused,
  ViewCellDiscarded,
  TargetUnknown,
  AxisUnknown,
  ViewKindTaken,
  ChordTaken,
  ViewSpaceRefused,
  PlaceUnknown,
  BindingsReplayed,
  SavedMemberGone,
  SavedNameGone,
  /// What a binding command did and the typed words it could not use, said as
  /// a reader would say them (U7a).
  AxisShows,
  NameUnknown,
  CommandUnclear,
  SubviewShown,
  NoSubviews,
  /// An action the presentation in front cannot do yet, said rather than
  /// swallowed while a new presentation stands beside the legacy one.
  ActionNotSupported,
  /// A binding that leaves one dimension on two points: allowed (§7.3), said.
  AxisAlsoShown,
  /// A view action sent while the ZigZag visualizer is in front, which has no
  /// views to bind or sub-views to switch.
  VisualizerCannot,
  /// A binding point that cannot be added.
  PointNameBad,
  /// A step on a point that shows nothing: unlike a dead end, it is fixed by
  /// binding something there.
  PointShowsNothing,
};

/// Every enumerator, in declaration order, so a test can visit each one.
inline constexpr std::array kViewErrors{
    ViewError::OccupiedDirection, ViewError::RealCellInViewLink,
    ViewError::StaleEpoch,        ViewError::UnknownTarget,
    ViewError::UnknownAxis,       ViewError::GroupCycle,
    ViewError::EmptyGroupBind,    ViewError::DuplicateViewKind,
    ViewError::ChordCollision,    ViewError::PromotionRefused,
    ViewError::ArenaRefused,      ViewError::UnknownPlace,
    ViewError::UnknownName,       ViewError::BadCommand,
    ViewError::BadPointName,
};

inline constexpr std::array kViewMessages{
    ViewMessage::NothingThatWay,      ViewMessage::NoPackThatWay,
    ViewMessage::RebindDiscarded,     ViewMessage::EmptyGroup,
    ViewMessage::GroupInsideItself,   ViewMessage::SavedBindingGone,
    ViewMessage::GroupRemovedFromUse, ViewMessage::PackTooLargeToKeep,
    ViewMessage::PagesStillArriving,  ViewMessage::DirectionOccupied,
    ViewMessage::RealCellRefused,     ViewMessage::ViewCellDiscarded,
    ViewMessage::TargetUnknown,       ViewMessage::AxisUnknown,
    ViewMessage::ViewKindTaken,       ViewMessage::ChordTaken,
    ViewMessage::ViewSpaceRefused,    ViewMessage::PlaceUnknown,
    ViewMessage::BindingsReplayed,    ViewMessage::SavedMemberGone,
    ViewMessage::SavedNameGone,       ViewMessage::AxisShows,
    ViewMessage::NameUnknown,         ViewMessage::CommandUnclear,
    ViewMessage::SubviewShown,        ViewMessage::NoSubviews,
    ViewMessage::ActionNotSupported,  ViewMessage::AxisAlsoShown,
    ViewMessage::VisualizerCannot,    ViewMessage::PointNameBad,
    ViewMessage::PointShowsNothing,
};

/// The message a refusal is reported with. The switch has no default, so an
/// enumerator added without a message does not compile (-Wswitch, -Werror).
[[nodiscard]] constexpr ViewMessage messageKey(ViewError error) noexcept {
  switch (error) {
  case ViewError::OccupiedDirection:
    return ViewMessage::DirectionOccupied;
  case ViewError::RealCellInViewLink:
    return ViewMessage::RealCellRefused;
  case ViewError::StaleEpoch:
    return ViewMessage::ViewCellDiscarded;
  case ViewError::UnknownTarget:
    return ViewMessage::TargetUnknown;
  case ViewError::UnknownAxis:
    return ViewMessage::AxisUnknown;
  case ViewError::GroupCycle:
    return ViewMessage::GroupInsideItself;
  case ViewError::EmptyGroupBind:
    return ViewMessage::EmptyGroup;
  case ViewError::DuplicateViewKind:
    return ViewMessage::ViewKindTaken;
  case ViewError::ChordCollision:
    return ViewMessage::ChordTaken;
  case ViewError::PromotionRefused:
    return ViewMessage::PackTooLargeToKeep;
  case ViewError::ArenaRefused:
    return ViewMessage::ViewSpaceRefused;
  case ViewError::UnknownPlace:
    return ViewMessage::PlaceUnknown;
  case ViewError::UnknownName:
    return ViewMessage::NameUnknown;
  case ViewError::BadCommand:
    return ViewMessage::CommandUnclear;
  case ViewError::BadPointName:
    return ViewMessage::PointNameBad;
  }
  return ViewMessage::ViewSpaceRefused;
}

/// A stable name for @p message: what a test, a log line or a translation
/// table keys on, so rewording a template breaks none of them.
[[nodiscard]] constexpr std::string_view
messageId(ViewMessage message) noexcept {
  switch (message) {
  case ViewMessage::NothingThatWay:
    return "view.nothingThatWay";
  case ViewMessage::NoPackThatWay:
    return "view.noPackThatWay";
  case ViewMessage::RebindDiscarded:
    return "view.rebindDiscarded";
  case ViewMessage::EmptyGroup:
    return "view.emptyGroup";
  case ViewMessage::GroupInsideItself:
    return "view.groupInsideItself";
  case ViewMessage::SavedBindingGone:
    return "view.savedBindingGone";
  case ViewMessage::GroupRemovedFromUse:
    return "view.groupRemovedFromUse";
  case ViewMessage::PackTooLargeToKeep:
    return "view.packTooLargeToKeep";
  case ViewMessage::PagesStillArriving:
    return "view.pagesStillArriving";
  case ViewMessage::DirectionOccupied:
    return "view.directionOccupied";
  case ViewMessage::RealCellRefused:
    return "view.realCellRefused";
  case ViewMessage::ViewCellDiscarded:
    return "view.viewCellDiscarded";
  case ViewMessage::TargetUnknown:
    return "view.targetUnknown";
  case ViewMessage::AxisUnknown:
    return "view.axisUnknown";
  case ViewMessage::ViewKindTaken:
    return "view.viewKindTaken";
  case ViewMessage::ChordTaken:
    return "view.chordTaken";
  case ViewMessage::ViewSpaceRefused:
    return "view.viewSpaceRefused";
  case ViewMessage::PlaceUnknown:
    return "view.placeUnknown";
  case ViewMessage::BindingsReplayed:
    return "view.bindingsReplayed";
  case ViewMessage::SavedMemberGone:
    return "view.savedMemberGone";
  case ViewMessage::SavedNameGone:
    return "view.savedNameGone";
  case ViewMessage::AxisShows:
    return "view.axisShows";
  case ViewMessage::NameUnknown:
    return "view.nameUnknown";
  case ViewMessage::CommandUnclear:
    return "view.commandUnclear";
  case ViewMessage::SubviewShown:
    return "view.subviewShown";
  case ViewMessage::NoSubviews:
    return "view.noSubviews";
  case ViewMessage::ActionNotSupported:
    return "view.actionNotSupported";
  case ViewMessage::AxisAlsoShown:
    return "view.axisAlsoShown";
  case ViewMessage::VisualizerCannot:
    return "view.visualizerCannot";
  case ViewMessage::PointNameBad:
    return "view.pointNameBad";
  case ViewMessage::PointShowsNothing:
    return "view.pointShowsNothing";
  }
  return {};
}

/// The words of @p message, with its fields in braces.
[[nodiscard]] constexpr std::string_view
messageText(ViewMessage message) noexcept {
  switch (message) {
  case ViewMessage::NothingThatWay:
    return "No cell further along {dimension} {direction}.";
  case ViewMessage::NoPackThatWay:
    return "{group} has nothing further {direction} from here.";
  case ViewMessage::RebindDiscarded:
    return "Showing {target} on axis {axis}. Discarded {count} view-only "
           "cells.";
  case ViewMessage::EmptyGroup:
    return "Group '{name}' has no dimensions yet.";
  case ViewMessage::GroupInsideItself:
    return "'{inner}' already contains '{outer}'.";
  case ViewMessage::SavedBindingGone:
    return "Dimension '{name}' is gone; axis {axis} is unbound.";
  case ViewMessage::GroupRemovedFromUse:
    return "Removed '{name}' from axes {axes} and groups {groups}.";
  case ViewMessage::PackTooLargeToKeep:
    return "This pack is too large to keep ({count} cells).";
  case ViewMessage::PagesStillArriving:
    return "{document}: {count} pages so far, still paginating.";
  case ViewMessage::DirectionOccupied:
    return "That view cell already has a neighbour {direction} along "
           "{dimension}; nothing was linked.";
  case ViewMessage::RealCellRefused:
    return "A view links only its own view-only cells; a cell of the slice "
           "was refused.";
  case ViewMessage::ViewCellDiscarded:
    return "That view-only cell was discarded when the view changed.";
  case ViewMessage::TargetUnknown:
    return "That is neither a cell of the slice nor a group of this view.";
  case ViewMessage::AxisUnknown:
    return "There is no axis {axis}.";
  case ViewMessage::ViewKindTaken:
    return "A view of kind '{kind}' is already installed.";
  case ViewMessage::ChordTaken:
    return "{chord} already runs {action}; '{kind}' was not installed.";
  case ViewMessage::ViewSpaceRefused:
    return "The view space refused the change.";
  case ViewMessage::PlaceUnknown:
    return "There is no place {place} in {where}.";
  case ViewMessage::BindingsReplayed:
    return "Restored {restored} bindings and groups; {missing} saved names "
           "are gone.";
  case ViewMessage::SavedMemberGone:
    return "'{name}' is gone; group '{group}' no longer contains it.";
  case ViewMessage::SavedNameGone:
    return "'{name}' is gone; it is no longer in {where}.";
  case ViewMessage::AxisShows:
    return "Showing {target} on axis {axis}.";
  case ViewMessage::NameUnknown:
    return "There is no dimension or group called '{name}' in this slice.";
  case ViewMessage::CommandUnclear:
    return "Say which point and what it shows, as in 'x d.across'.";
  case ViewMessage::SubviewShown:
    return "Showing {view}, {subview} ({place} of {count}).";
  case ViewMessage::NoSubviews:
    return "{view} has no sub-views.";
  case ViewMessage::ActionNotSupported:
    return "{view} cannot {action} yet; the ZigZag visualizer can, from the "
           "view palette.";
  case ViewMessage::AxisAlsoShown:
    return "Showing {target} on axis {axis}; axis {other} shows it too.";
  case ViewMessage::VisualizerCannot:
    return "The ZigZag visualizer cannot {action}; choose a slice view from "
           "the view palette first.";
  case ViewMessage::PointNameBad:
    return "A binding point needs a name of its own; '{name}' will not do.";
  case ViewMessage::PointShowsNothing:
    return "{axis} shows nothing; bind a dimension to it first.";
  }
  return {};
}

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_VIEW_ERROR_HPP

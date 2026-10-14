/**
 * @file view_commands.hpp
 * @brief The logic of the view commands that need no device: a binding typed
 *        by name, and stepping through a view's sub-views
 * (design/view-system.md §7.6, §12.1; plan §4.6 U7a).
 *
 * A command's chord and its registered name are the keymap's business
 * (system://keymap); what a command does to a placement lives here, so a
 * front end only routes the name and says the outcome.
 */
#ifndef COMMON_XANADU_VIEW_VIEW_COMMANDS_HPP
#define COMMON_XANADU_VIEW_VIEW_COMMANDS_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_binding.hpp"
#include "common/xanadu/view/view_error.hpp"
#include "common/xanadu/view/view_ids.hpp"
#include "common/xanadu/view/view_manifold.hpp"

namespace xanadu {
class SpanReader;
} // namespace xanadu

namespace xanadu::view {

/// A binding as a reader types it: a point's name, then what it should show,
/// which may have spaces in it ("x d.across", "y Contact details").
struct BindCommand {
  std::string point, target;

  bool operator==(const BindCommand &) const = default;
};

/// Nothing for text that does not name both a point and a target.
[[nodiscard]] std::optional<BindCommand>
parseBindCommand(std::string_view text);

/// The binding point a reader means by @p name: the one spelt exactly so, or
/// the only one that differs from it in case ('X' for x). Nothing for a name
/// that fits none, or fits two.
[[nodiscard]] std::optional<ViewAxisId>
bindingPointNamed(const ViewAxisSet &axes, std::string_view name);

/// What a reader may type in a binding: the points, then the slice's
/// dimensions in the order the slice lists them, then the groups made for it.
/// The slice's own list of dimensions is left out, as bindByName() refuses it.
struct BindingChoices {
  std::vector<std::string> points, dimensions, groups;
};

[[nodiscard]] BindingChoices bindingChoicesOf(const ViewManifold &space,
                                              const SpanReader &reader);

/// What a typed binding did.
struct Bound {
  ViewAxisId axis{};
  /// The point's name, and the name of what it now shows.
  std::string point, shows;
  bool group{};
};

/**
 * @brief Bind the dimension or group @p command names to the point it names.
 *
 * A dimension is found by its name in the base, read through @p reader; a
 * group by its name among the live groups. A dimension wins a name both have,
 * because the slice's words are what a reader typing a name has in front of
 * them. The slice's own list of dimensions is never bindable (d.dims is the
 * plumbing a view is reached through, not something to look along). The
 * caller tosses afterwards (§7.4).
 *
 * @return UnknownAxis for a point that is not configured, UnknownName for a
 *         name that is neither, and whatever ViewAxisSet::bind refuses
 *         (EmptyGroupBind).
 */
[[nodiscard]] std::expected<Bound, ViewError>
bindByName(ViewManifold &space, const SpanReader &reader,
           const BindCommand &command);

/// The sub-view @p by places after @p current in a view of @p count
/// sub-views, wrapping at both ends. Nothing for a view with none.
[[nodiscard]] std::optional<std::uint32_t>
steppedSubview(std::size_t count, std::uint32_t current, int by) noexcept;

/// The index of the sub-view @p descriptor gives @p id; nothing for an id it
/// does not have, and for the empty id nothing too, which stands for the
/// view's first.
[[nodiscard]] std::optional<std::uint32_t>
subviewIndexOf(const ViewDescriptor &descriptor, std::string_view id);

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_VIEW_COMMANDS_HPP

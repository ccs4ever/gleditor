/**
 * @file link_views.hpp
 * @brief Predicates over Xanadu links, for std::views::filter.
 *
 * A store's links are a table keyed by id, and every consumer wants some
 * slice of it: the format links, the links that are not format links, the
 * ones touching a span. Each used to be a loop with a `continue` at the top.
 * Spelled as predicates they compose -- `store.linkView() |
 * views::filter(ofType(Comment)) | views::filter(touching(span))` -- and the
 * reader sees what is kept rather than what is skipped.
 *
 * Every predicate captures by value, so a pipeline built from them may
 * outlive the arguments it was built with.
 */
#ifndef COMMON_XANADU_LINK_VIEWS_HPP
#define COMMON_XANADU_LINK_VIEWS_HPP

#include <string>
#include <string_view>

#include "common/xanadu/ops.hpp"
#include <gleditor/ranges.hpp>

namespace xanadu {
using gleditor::firstOf;
using gleditor::lastOf;
} // namespace xanadu

namespace xanadu::links {

/// Links of type @p type.
[[nodiscard]] constexpr auto ofType(const LinkType type) noexcept {
  return [type](const Link &link) noexcept { return type == link.type; };
}

/// Links of any type but @p type.
[[nodiscard]] constexpr auto notOfType(const LinkType type) noexcept {
  return [type](const Link &link) noexcept { return type != link.type; };
}

/// Links with an end covering any of @p span. Link::touches as a predicate.
[[nodiscard]] inline auto touching(const PrimediaSpan &span) noexcept {
  return [span](const Link &link) { return link.touches(span); };
}

/// Links made by @p owner.
[[nodiscard]] inline auto ownedBy(std::string_view owner) {
  return [owner = std::string{owner}](const Link &link) {
    return owner == link.owner;
  };
}

} // namespace xanadu::links

#endif // COMMON_XANADU_LINK_VIEWS_HPP

/**
 * @file view.hpp
 * @brief Views, their descriptors, and the registry a host installs them in.
 *
 * design/view-system.md §8.1 and §8.10. A view is installed by one call to
 * ViewRegistry::add() with nothing else to edit: no enum, switch or list
 * names a view kind anywhere in the framework (V-R1), so a built-in view and
 * a third-party one arrive the same way. The registry is an object its host
 * owns, not a singleton.
 *
 * A descriptor carries the view's settings and default chords, which the
 * host seeds into system://settings and system://keymap, and its sub-views,
 * which the reader cycles and which reach layout() as an index (V-R49, V26).
 * add() refuses a chord that an action already holds in the same scope,
 * because two actions on one chord leave one of them unreachable from the
 * keyboard.
 */
#ifndef COMMON_XANADU_VIEW_VIEW_HPP
#define COMMON_XANADU_VIEW_VIEW_HPP

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/view_error.hpp"

namespace xanadu::view {

/// What a view lays out.
enum class ViewSubject : std::uint8_t { Slice, Page };

/// A layout and interaction strategy. One instance per placement, so an
/// instance may keep caches; it keeps no cursor and no binding.
class View {
public:
  View()                                                       = default;
  View(const View &)                                           = delete;
  View &operator=(const View &)                                = delete;
  View(View &&)                                                = delete;
  View &operator=(View &&)                                     = delete;
  virtual ~View()                                              = default;
  [[nodiscard]] virtual std::string_view kind() const noexcept = 0;
};

/// One default binding: @p action names it, @p call is the Vortex call it
/// dispatches, @p chord is the default key and @p context the keymap scope
/// it is live in, as keymapScope() answers them: empty for anywhere.
struct ChordSpec {
  std::string action, call, chord, context;
};

/// A variant of a view the reader cycles through (V-R49).
struct SubviewSpec {
  std::string id, name;
};

struct ViewDescriptor {
  std::string kind; // "slice.stretch-vanishing", "page.base"
  std::string name, description, glyph;
  ViewSubject subject{};
  /// Variants the reader cycles through (V-R49). The first is the default.
  std::vector<SubviewSpec> subviews;
  std::vector<SettingSpec> settings;
  std::vector<ChordSpec> chords;
  /// A std::function because the descriptor outlives the call that
  /// registered it; a function_ref would dangle.
  std::function<std::unique_ptr<View>()> make;
};

class ViewRegistry {
public:
  /// Holds the default keymap's chords from the start, so a view cannot take
  /// one that an existing action already has.
  ViewRegistry();

  /// Refuses a second descriptor of the same kind (DuplicateViewKind) and a
  /// default chord that is already taken in its scope, by an action or by
  /// another chord of the same descriptor (ChordCollision). A refused
  /// descriptor leaves the registry as it was.
  std::expected<ViewRegistry *, ViewError> add(ViewDescriptor descriptor);

  /// In the order added. Adding a view invalidates earlier spans and
  /// references.
  [[nodiscard]] std::span<const ViewDescriptor> views() const noexcept {
    return views_;
  }
  [[nodiscard]] gleditor::cpp26::optional<const ViewDescriptor &>
  find(std::string_view kind) const noexcept;

  /// The call that holds @p chord in @p context, for the words of a
  /// ChordCollision.
  [[nodiscard]] std::optional<std::string_view>
  chordHolder(std::string_view chord, std::string_view context) const;

private:
  std::vector<ViewDescriptor> views_;
  /// (scope, canonical chord) to the call that holds it.
  std::map<std::pair<std::string, std::string>, std::string, std::less<>>
      chords_;
};

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_VIEW_HPP

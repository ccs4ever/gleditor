/**
 * @file view.hpp
 * @brief Base interfaces and contracts for pluggable presentation views.
 *
 * Implements the core View and ViewDescriptor types separating mathematical
 * space projection from physical rendering, in accordance with
 * design/view-system.md.
 */
#ifndef XANADU_VIEW_VIEW_HPP
#define XANADU_VIEW_VIEW_HPP

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/view/view_records.hpp"
#include <gleditor/cpp26.hpp>

namespace xanadu::view {

enum class ViewSubject : std::uint8_t { Slice, Page };

class View {
public:
  virtual ~View()                                              = default;
  [[nodiscard]] virtual std::string_view kind() const noexcept = 0;
};

struct ChordSpec {
  std::string action;
  std::string call;
  std::string chord;
  std::string context;
};

struct SubviewSpec {
  std::string id;
  std::string name;
};

struct ViewDescriptor {
  std::string kind;
  std::string name;
  std::string description;
  std::string glyph;
  ViewSubject subject{ViewSubject::Slice};
  std::vector<SubviewSpec> subviews;
  std::vector<ChordSpec> chords;
  std::function<std::unique_ptr<View>()> make;
};

class ViewRegistry {
public:
  ViewRegistry() = default;

  std::expected<ViewRegistry *, ViewError> add(ViewDescriptor descriptor) {
    for (const auto &v : views_) {
      if (v.kind == descriptor.kind) {
        return std::unexpected(ViewError::CellNotFound); // Duplicate
      }
    }
    views_.push_back(std::move(descriptor));
    return this;
  }

  [[nodiscard]] std::span<const ViewDescriptor> views() const noexcept {
    return views_;
  }

  [[nodiscard]] std::optional<std::reference_wrapper<const ViewDescriptor>>
  find(std::string_view kind) const noexcept {
    for (const auto &v : views_) {
      if (v.kind == kind) {
        return v;
      }
    }
    return std::nullopt;
  }

private:
  std::vector<ViewDescriptor> views_;
};

} // namespace xanadu::view

#endif // XANADU_VIEW_VIEW_HPP

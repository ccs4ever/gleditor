/**
 * @file slice_presentation.hpp
 * @brief The seam between xuzz and whatever presents its ZigZag slice.
 */
#ifndef COMMON_UI_VIEW_SLICE_PRESENTATION_HPP
#define COMMON_UI_VIEW_SLICE_PRESENTATION_HPP

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_float3.hpp>

#include <gleditor/ui/focus_manager.hpp>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/presentation_surface.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"

namespace xanadu {
class Store;
} // namespace xanadu

namespace xanadu::view {

/**
 * @brief Everything the application and its view coordinator ask of a slice
 *        presentation, so a second presentation can stand beside the legacy
 *        visualizer without the application knowing which one it holds
 *        (view-system-implementation-plan.md §4.6, U0).
 *
 * The bridge half -- focus, anchors, highlights, lock state and the drawing
 * roles -- is the ZigzagPresentationSurface a BridgeCoordinator attaches; this
 * adds what xuzz itself calls. The command bar, the palettes, cell editing and
 * the Vortex host are not here: they belong to the legacy visualizer until X0
 * carves them into components every presentation shares.
 */
class SlicePresentation : public ZigzagPresentationSurface {
public:
  /// A null result keeps the presentation hidden while its host page has not
  /// been built yet; resolved once per frame.
  using PresentationTransformResolver =
      std::function<std::optional<glm::mat4>()>;
  using ExternInspector = std::function<std::string(zigzag::CellRef)>;

  SlicePresentation()                                     = default;
  SlicePresentation(const SlicePresentation &)            = delete;
  SlicePresentation &operator=(const SlicePresentation &) = delete;
  SlicePresentation(SlicePresentation &&)                 = delete;
  SlicePresentation &operator=(SlicePresentation &&)      = delete;
  ~SlicePresentation() override                           = default;

  // -- Composition ----------------------------------------------------------
  /// The input scope the application registers with its focus manager.
  [[nodiscard]] virtual gleditor::ui::FocusScope *focusScope() noexcept = 0;
  /// Hides the slice without unbinding its store or losing its focus.
  virtual SlicePresentation *setPresentationVisible(bool visible)    = 0;
  [[nodiscard]] virtual bool presentationVisible() const noexcept    = 0;
  virtual SlicePresentation *setPresentationOrigin(glm::vec3 origin) = 0;
  virtual SlicePresentation *
  setPresentationTransformResolver(PresentationTransformResolver resolver) = 0;
  /// One validated `system://layout` snapshot, applied between frames.
  virtual SlicePresentation *
  setPresentationConfig(ZigzagPresentationConfig config) = 0;
  [[nodiscard]] virtual const ZigzagPresentationConfig &
  presentationConfig() const noexcept = 0;

  // -- The slice shown ------------------------------------------------------
  virtual void bindXuduStore(Store &store, const MicroversionId &version) = 0;
  [[nodiscard]] virtual Store *store() const noexcept                     = 0;
  /// The version the slice shown was folded at; zero for none.
  [[nodiscard]] virtual MicroversionId sliceHead() const = 0;
  /// The legacy three-axis binding, which the satelloid overlay walks.
  [[nodiscard]] virtual const zigzag::ViewAxisBinding &currentView() const = 0;

  // -- Reading --------------------------------------------------------------
  /// Shows a chosen link occurrence outside the focused neighbourhood without
  /// moving the reader's focus or recording a visit.
  virtual SlicePresentation *
  setPreviewCell(std::optional<zigzag::CellRef> cell) = 0;
  /// What the accessibility description says of a cell that stands for a
  /// cell in another store; the application knows the other stores.
  virtual SlicePresentation *setExternInspector(ExternInspector inspector) = 0;

  // -- Keyboard -------------------------------------------------------------
  /// Called from the event thread.
  virtual SlicePresentation *setHasKeyboard(bool has) noexcept = 0;
  /// The hint line built from the bindings the application actually has:
  /// @p here while the slice has the keyboard, @p elsewhere while not.
  virtual SlicePresentation *setKeyHints(std::string here,
                                         std::string elsewhere) = 0;

  // -- Actions --------------------------------------------------------------
  /// Runs a keymap action the application has no command of its own for.
  /// @return false for an action this presentation does not support.
  virtual bool dispatchAction(std::string_view actionName) = 0;
  /// Adds a cell holding @p spans after the focus, along the dimension bound
  /// to the horizontal axis, and focuses it.
  virtual bool
  insertConnectedTransclusion(std::span<const PrimediaSpan> spans) = 0;
};

} // namespace xanadu::view

#endif // COMMON_UI_VIEW_SLICE_PRESENTATION_HPP

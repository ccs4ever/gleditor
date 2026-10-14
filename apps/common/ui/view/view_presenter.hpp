/**
 * @file view_presenter.hpp
 * @brief Layout records to drawing: flat placements on world canvases and
 *        beams, the text::fit measurer, picks and accessibility bounds
 *        (design/view-system.md §8.9, §13, §15; plan §4.1, §4.6 U2).
 *
 * The spine does not wait for the library (plan §4.1): a flat placement is
 * drawn with what exists today, as the ZigZag visualizer draws, on a world
 * canvas for cell content, a second world canvas for secondary text, and a
 * screen canvas for chrome, with edges as beams. A placement whose items lie
 * in several depth planes gets one pair of world canvases per plane, each
 * drawn through its plane's offset; turning planes towards the camera waits
 * for PlaneSet (U4a).
 *
 * Retained by stamp: geometry is rebuilt and uploaded only when the records'
 * stamp or the typography changes. A frame in which neither did draws what is
 * already on the device, shapes nothing and uploads nothing (V-R44). Text is
 * fitted through one ShapingCache, so a rebuild after a move re-shapes only
 * text it has not seen.
 *
 * Kinds of record the presenter does not know are skipped, as §8.10 asks.
 */
#ifndef COMMON_UI_VIEW_VIEW_PRESENTER_HPP
#define COMMON_UI_VIEW_VIEW_PRESENTER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/beams.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/cpp26.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text/shaping_cache.hpp>
#include <gleditor/ui/metrics.hpp>
#include <gleditor/ui/theme.hpp>
#include <gleditor/ui/world_panel.hpp>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/compass.hpp"
#include "common/xanadu/view/dimension_colour.hpp"
#include "common/xanadu/view/view_error.hpp"
#include "common/xanadu/view/view_marks.hpp"
#include "common/xanadu/view/view_records.hpp"
#include "common/xanadu/zigzag/presentation_surface.hpp"

namespace render {
class RenderDevice;
} // namespace render

namespace xanadu::view {

/// One field of a message template: {name} is replaced by value.
struct MessageField {
  std::string_view name;
  std::string_view value;
};

/// @p message's words with its fields filled, for the screen and for an
/// announcement alike: one place, so the two never word it differently.
[[nodiscard]] std::string spokenMessage(ViewMessage message,
                                        std::span<const MessageField> fields);

/// @p style's dash pattern as alternating drawn and open lengths, in beam
/// widths, from @p draw (view.draw.dash.*); empty for solid. The dash
/// vocabulary of U1, drawn.
[[nodiscard]] std::span<const float> dashPattern(const ViewDrawConfig &draw,
                                                 DashStyle style) noexcept;

/// What a placement looks like to the presenter for one frame.
struct PresentedPlacement {
  const LayoutSink &records;
  /// Changes whenever the records do: equal stamps draw what is retained.
  std::uint64_t stamp{};
  /// Placement-local to world.
  glm::mat4 toWorld{1.0F};
  /// What a pick on a cell names: the store and the state it was drawn at.
  std::string_view documentId;
  std::string_view version;
  /// Cell padding and the content width limit, in local units, as the layout
  /// used them.
  float paddingX{}, paddingY{}, contentMaxWidth{};
  /// An edge's width, in local units.
  float edgeWidth{};
  /// Pixels a cell must clear the pane's edge by before it is drawn with its
  /// content again (§9.1.5): hysteresis on the all-or-nothing clip.
  float clipMarginPx{};
  /// What a cell held back by that hysteresis is drawn at, as a ghost: the
  /// view's own ghost opacity, so it never outshines a real cell (D1).
  float ghostOpacity{};
  /// Cells holding part of the host's selected link, and their border.
  std::span<const CellHighlight> highlights;
  std::uint32_t highlightBorder{};
};

/// The words and colours a frame's subjects carry. Called, not kept.
struct PresentedWords {
  /// A subject's full text: a cell's content, a label's name.
  gleditor::cpp26::function_ref<std::string_view(SubjectId)> text;
  /// The cue of the dimension an edge's relation names.
  gleditor::cpp26::function_ref<DimensionCue(std::uint64_t)> cue;
};

/// The chrome strip under the scene: what is not in the world (§11.7).
struct PresentedChrome {
  /// The last cells walked, oldest first (§9.1.6).
  std::span<const std::string> breadcrumbs;
  /// The latest message, empty for none.
  std::string_view notice;
  /// The hint line built from the reader's own bindings.
  std::string_view hints;
  /// The strip's padding, in logical pixels: zigzag.hud* (§12.3).
  float paddingX{}, paddingY{};
  /// Changes whenever any of the above does.
  std::uint64_t stamp{};
};

/// The compass for one frame: a row for each binding point, in the corner of
/// the pane (view-system.md §7.9).
struct PresentedCompass {
  std::span<const CompassArm> arms;
  CompassConfig config;
  /// Changes whenever the arms or the config do.
  std::uint64_t stamp{};
};

class SlicePresenter {
public:
  /// @p fontOverride is the font description the host gives every
  /// presentation, empty for the typography roles' own faces.
  explicit SlicePresenter(std::string fontOverride = {});
  SlicePresenter(const SlicePresenter &)            = delete;
  SlicePresenter &operator=(const SlicePresenter &) = delete;
  SlicePresenter(SlicePresenter &&)                 = delete;
  SlicePresenter &operator=(SlicePresenter &&)      = delete;
  ~SlicePresenter();

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline);
  [[nodiscard]] bool ready() const noexcept { return device_ != nullptr; }

  /// How strokes, dashes and strands are drawn (view.draw.*, view.strand.*);
  /// what was built is rebuilt when it changes. Chains.
  SlicePresenter *setDrawConfig(const ViewDrawConfig &draw);
  [[nodiscard]] const ViewDrawConfig &drawConfig() const noexcept {
    return draw_;
  }

  /// Read the frame's metrics and theme. True when typography changed, which
  /// changes what measure() answers, so the host lays out again.
  bool setTypography(const gleditor::ui::UiMetrics &metrics,
                     const gleditor::ui::Theme &theme);
  [[nodiscard]] const gleditor::ui::UiMetrics &metrics() const noexcept {
    return metrics_;
  }
  [[nodiscard]] const gleditor::ui::Theme &theme() const noexcept {
    return theme_;
  }
  /// One line of cell content, in canvas pixels.
  [[nodiscard]] float bodyLineHeight() const noexcept {
    return bodyFont_ ? bodyFont_->metrics().lineHeight : 0.0F;
  }
  /// Changes with every typography change.
  [[nodiscard]] std::uint64_t typographyStamp() const noexcept {
    return typographyStamp_;
  }

  /**
   * @brief The measurer a layout is given (§8.4): @p text's size in
   *        @p subject's font role at @p maxWidth, through text::fit.
   *
   * Cell content is Body, wrapped; a label or badge is Caption, one line.
   * Cached, so measuring what a layout measured before shapes nothing.
   */
  [[nodiscard]] ContentExtent measure(SubjectId subject, float maxWidth,
                                      std::string_view text);

  /// Draw the placement into the scene. Rebuilds only on a new stamp or new
  /// typography; hands its beams to the frame's translucent list when there
  /// is one, so they are sorted with faded pages.
  void drawScene(gleditor::FrameContext &ctx,
                 const PresentedPlacement &placement,
                 const PresentedWords &words);
  /// Draw the chrome strip along the bottom edge and claim its band.
  void drawChrome(gleditor::FrameContext &ctx, const PresentedChrome &chrome);
  /// Draw the compass in the top left corner, over the scene. It claims no
  /// band, and keeps clear of the chrome other contributors have claimed.
  void drawCompass(gleditor::FrameContext &ctx,
                   const PresentedCompass &compass);

  // -- what was drawn ------------------------------------------------------
  /// The subject a pick landed on, if it was one of this frame's: a pick
  /// requested before the records changed names nothing.
  [[nodiscard]] std::optional<SubjectId>
  subjectOf(const render::PickingResult &pick) const;
  /// The records last drawn, in the order of their accessibility nodes.
  [[nodiscard]] std::span<const PlacedItem> drawnItems() const noexcept {
    return drawn_;
  }
  /// What item @p index was drawn with, after the clip's hysteresis.
  [[nodiscard]] ContentMode drawnContent(std::size_t index) const;
  /// Whether item @p index was drawn as a ghost, by the layout or by the
  /// clip's hysteresis.
  [[nodiscard]] bool drawnAsGhost(std::size_t index) const;
  /// Where item @p index lies on screen, top-down, for its node's bounds.
  [[nodiscard]] std::optional<gleditor::a11y::Rect>
  screenBounds(std::size_t index) const;
  /// Changes whenever an item's screen bounds do.
  [[nodiscard]] std::uint64_t projectionRevision() const noexcept {
    return projectionRevision_;
  }

  /// The strands as last committed: one row for each piece drawn, after the
  /// labels were cut out of them and each dash made its own.
  [[nodiscard]] std::span<const gleditor::Beams::Row> strands() const {
    return beams_ ? beams_->committedBeams()
                  : std::span<const gleditor::Beams::Row>{};
  }

  // -- counters for the tests that hold the presenter to V-R44 ---------------
  [[nodiscard]] std::uint64_t rebuilds() const noexcept { return rebuilds_; }
  [[nodiscard]] gleditor::text::ShapingCache::Stats shapingStats() const {
    return shaping_.stats();
  }

private:
  /// One depth plane's two world canvases.
  struct Plane {
    float z{};
    std::unique_ptr<gleditor::Canvas> body, caption;
    bool used{};
  };

  [[nodiscard]] const gleditor::text::FontFacePtr &
  fontFor(SubjectKind kind) const noexcept;
  [[nodiscard]] const gleditor::text::FittedText &
  contentText(std::string_view text, float maxWidth);
  [[nodiscard]] const gleditor::text::FittedText &
  oneLine(std::string_view text, const gleditor::text::FontFacePtr &font,
          float maxWidth);
  Plane &planeAt(float z);
  void rebuild(gleditor::FrameContext &ctx, const glm::mat4 &toClip,
               const PresentedPlacement &placement,
               const PresentedWords &words);
  void drawCell(RenderState &state, Plane &plane, const PlacedItem &item,
                ContentMode content, std::uint32_t border, float borderPx,
                std::string_view text, const PresentedPlacement &placement);
  /// @p px logical pixels as a length at @p at, in the units the placement
  /// is laid out in, so that it is that many device pixels on the screen.
  [[nodiscard]] float deviceStroke(float px, const glm::vec3 &at) const;
  void drawGhost(Plane &plane, const PlacedItem &item);
  void drawLabel(RenderState &state, Plane &plane, const PlacedItem &item,
                 std::string_view text, std::optional<std::uint32_t> rule);
  void addEdge(const PlacedEdge &edge, const PresentedPlacement &placement,
               const PresentedWords &words, float pxPerUnit);
  /// Add the part of @p edge between lengths @p from and @p to, minus the
  /// label boxes from index @p label on, which a strand stops short of.
  void addStrand(const PlacedEdge &edge, glm::vec3 along, float length,
                 float width, std::uint32_t colour,
                 std::span<const float> pattern, float from, float to,
                 std::size_t label);
  /// A beam of @p edge from length @p from to @p to, dashed by @p pattern.
  void addDashed(const PlacedEdge &edge, glm::vec3 along, float length,
                 float width, std::uint32_t colour,
                 std::span<const float> pattern, float from, float to);
  [[nodiscard]] std::shared_ptr<const render::PickSemanticTarget>
  targetFor(zigzag::CellRef cell, const PresentedPlacement &placement);
  void project(const glm::mat4 &toClip, gleditor::ui::Size viewport);

  std::string fontOverride_;
  ViewDrawConfig draw_;
  render::RenderDevice *device_{};
  render::PipelineDesc pipeline_{};
  gleditor::ui::UiMetrics metrics_;
  gleditor::ui::Theme theme_;
  std::uint64_t typographyStamp_{1};
  gleditor::text::FontFacePtr bodyFont_, captionFont_, chromeFont_;
  std::string bodyName_, captionName_, chromeName_;
  mutable gleditor::text::ShapingCache shaping_;

  glm::mat4 toClip_{1.0F};
  float screenWidth_{1.0F};
  std::vector<Plane> planes_;
  std::unique_ptr<gleditor::Beams> beams_;
  std::unique_ptr<gleditor::Canvas> chrome_;
  std::unique_ptr<gleditor::Canvas> compass_;

  // What the retained geometry was built from.
  std::optional<std::uint64_t> drawnStamp_;
  std::uint64_t drawnTypography_{};
  std::vector<PlacedItem> drawn_;
  std::vector<ContentMode> drawnContent_;
  std::vector<bool> drawnGhost_;
  /// Cells drawn with their content in the last generation, for the clip's
  /// hysteresis: one not among them must clear the margin to show again.
  /// Kept sorted by (kind, value), and swapped with shownNext_ so a rebuild
  /// reuses both vectors' storage.
  std::vector<SubjectId> shownWithContent_, shownNext_;
  std::uint32_t pickScope_{};
  std::vector<std::shared_ptr<const render::PickSemanticTarget>> targets_;
  /// A pick names the cell it landed on in a document at a version, which
  /// does not change from one animating frame to the next: one target per
  /// cell, kept while the document and version stand, so a rebuild makes
  /// none.
  std::unordered_map<zigzag::CellRef,
                     std::shared_ptr<const render::PickSemanticTarget>>
      targetsByCell_;
  std::string targetsDocument_, targetsVersion_;
  /// The relation an edge names with the label at each item index.
  std::vector<std::optional<std::uint64_t>> labelRelation_;
  /// Label boxes of the plane being drawn, which strands stop short of.
  struct LabelBox {
    glm::vec2 lo, hi;
    float z{};
  };
  std::vector<LabelBox> labelBoxes_;

  glm::mat4 projectedWith_{0.0F};
  gleditor::ui::Size projectedViewport_{};
  std::vector<std::optional<gleditor::ui::ProjectedPlane>> projected_;
  std::uint64_t projectionRevision_{1};

  std::optional<std::uint64_t> chromeStamp_;
  gleditor::ui::UiMetrics chromeMetrics_;
  gleditor::ScreenInsets chromeBase_;
  float chromeClaim_{};

  std::optional<std::uint64_t> compassStamp_;
  gleditor::ui::UiMetrics compassMetrics_;
  gleditor::ScreenInsets compassBase_;
  bool compassDrawn_{};

  std::uint64_t rebuilds_{};
};

} // namespace xanadu::view

#endif // COMMON_UI_VIEW_VIEW_PRESENTER_HPP

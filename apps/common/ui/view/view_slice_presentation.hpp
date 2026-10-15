/**
 * @file view_slice_presentation.hpp
 * @brief The second slice presentation: one placement of a registered slice
 *        view, behind the seam xuzz already holds its slice through
 *        (view-system-implementation-plan.md §4.6, U2; view-system.md §8.8,
 *        §8.9).
 *
 * It holds what view-system.md §8.8 calls a SlicePlacement — a ViewManifold
 * over the slice, a SliceCursor, one SliceView and a LayoutSink — and runs
 * §8.9's pipeline: a command moves the cursor on the owner thread and marks
 * the placement stale; a frame in which it is stale prepares and lays out;
 * the presenter draws what the records say, rebuilding geometry only when
 * they changed.
 *
 * It stands beside the legacy visualizer until cut-over (X3), so it answers
 * every call of the seam. What it cannot do yet — editing, the omnibar, the
 * palette, bundles — it says it cannot: dispatchAction() answers false and
 * the chrome and an announcement carry ActionNotSupported's words.
 *
 * The compass (U5a) shows each binding point and what it is bound to, in the
 * corner of the pane; the view commands (U7a) are keymap actions it receives
 * by name: steps, home, hops along x, swapping x and y, the next and previous
 * sub-view, and a binding typed as 'x d.across'.
 *
 * Settings: the view's descriptor's settings are seeded into
 * system://settings when the presentation is made (view-system.md §8.1 gives
 * that to the host), and a dimension's cue is minted in system://ui the first
 * time it is bound (plan §5.2).
 */
#ifndef COMMON_UI_VIEW_VIEW_SLICE_PRESENTATION_HPP
#define COMMON_UI_VIEW_VIEW_SLICE_PRESENTATION_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/ui/focus_manager.hpp>

#include "common/ui/view/slice_presentation.hpp"
#include "common/ui/view/view_animation.hpp"
#include "common/ui/view/view_presenter.hpp"
#include "common/xanadu/link_navigation.hpp"
#include "common/xanadu/store_activity_log.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/compass.hpp"
#include "common/xanadu/view/dimension_colour.hpp"
#include "common/xanadu/view/slice/stretch_vanishing_view.hpp"
#include "common/xanadu/view/slice_view.hpp"
#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_binding.hpp"
#include "common/xanadu/view/view_commands.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/view/view_records.hpp"

namespace xanadu::view {

class ViewSlicePresentation final : public SlicePresentation,
                                    public gleditor::FrameContributor,
                                    public gleditor::PickObserver,
                                    public gleditor::a11y::Source,
                                    public gleditor::ui::FocusScope {
public:
  /// What the presentation needs of the program it runs in.
  struct Host {
    /// Run a task on the thread that owns the presentation: the render
    /// thread in xuzz. Empty: everything already runs on one thread.
    std::function<void(std::function<void()>)> post;
    /// The system xanadoc of a kind, or null where there is none.
    std::function<Store *(SystemDocKind)> systemStore;
    /// Make @p version of a system xanadoc current and say so, as
    /// Session::repointSystemDoc() does.
    std::function<void(SystemDocKind, const MicroversionId &)> repoint;
    /// Where the reader's resting place in each slice is kept: the activity
    /// store, not a setting (view-system.md §7.5). Empty: not kept.
    std::shared_ptr<SliceFocusLog> focus;
  };

  /// One placement of the slice view @p kind of @p registry, which must
  /// outlive this. Throws std::invalid_argument for a kind that is not a
  /// slice view in it.
  ViewSlicePresentation(const ViewRegistry &registry, std::string_view kind,
                        Host host, std::string fontOverride = {});
  ViewSlicePresentation(const ViewSlicePresentation &)            = delete;
  ViewSlicePresentation &operator=(const ViewSlicePresentation &) = delete;
  ViewSlicePresentation(ViewSlicePresentation &&)                 = delete;
  ViewSlicePresentation &operator=(ViewSlicePresentation &&)      = delete;
  ~ViewSlicePresentation() override;

  // -- the view
  // ----------------------------------------------------------------
  [[nodiscard]] std::string_view viewKind() const noexcept {
    return descriptor_.kind;
  }
  [[nodiscard]] std::string_view viewName() const noexcept {
    return descriptor_.name;
  }
  /// Read system://settings, system://layout and system://ui again: the
  /// view's settings, the binding points, and the dimension cues.
  ViewSlicePresentation *applySettings();
  /// Bind the axes to these dimensions by name, as the legacy visualizer
  /// shows them, so a reader switching presentations keeps the same axes.
  ViewSlicePresentation *adoptAxes(const zigzag::ViewAxisBinding &names);
  /// The sub-view showing: an index into the descriptor's, and its id.
  [[nodiscard]] std::uint32_t subview() const noexcept { return subview_; }
  [[nodiscard]] std::string_view subviewId() const noexcept;
  /// Show the sub-view @p by places on, wrapping; says which is showing, or
  /// that the view has none. The choice is kept with the slice in
  /// system://layout, so it survives a restart.
  ViewSlicePresentation *cycleSubview(int by);
  /// Bind what @p command names ('x d.across', 'y Contact') to its point, or
  /// say why not (notice() has the words). True when the binding was made.
  /// Runs on the owner thread; dispatchAction() is the way from any other.
  bool bindTyped(std::string_view command);
  /// What a reader can type in a binding: the points, the slice's dimensions
  /// and its groups. Empty until a slice is bound.
  [[nodiscard]] BindingChoices bindingChoices() const;
  /// What each binding point is bound to, as the compass shows it.
  [[nodiscard]] std::span<const CompassArm> compass() const;
  /// The placement's binding arena, for the host and its tests; none until a
  /// slice is bound.
  [[nodiscard]] ViewAxisSet *axisSet() const;
  /// Where completed moves are recorded and the breadcrumbs are read from.
  /// Not owned; null keeps them in a log of the presentation's own.
  ViewSlicePresentation *setActivityLog(ActivityLog *log);
  /// Keep where the reader is now as the slice's resting place, unless that
  /// is already kept. Called by the host when the reader leaves (a view
  /// change, the program ending) so a move made just before is not lost; a
  /// walk that comes to rest does it itself.
  ViewSlicePresentation *flushFocus();

  // -- what it shows: for the host's tests and the frame inspector
  // ---------------
  [[nodiscard]] const SliceCursor &cursor() const noexcept { return cursor_; }
  [[nodiscard]] const LayoutSink &records() const noexcept { return sink_; }
  [[nodiscard]] const SlicePresenter &presenter() const noexcept {
    return presenter_;
  }
  /// The latest message, empty for none.
  [[nodiscard]] std::string notice() const;
  /// The rows under the message and the walk as the strip last drew them:
  /// the view and sub-view showing, then the key hints.
  [[nodiscard]] std::string_view chromeRows() const noexcept {
    return chromeRows_;
  }
  /// What the placement says when it has no message: the view and the
  /// sub-view it is showing, "Stretch vanishing: Plain".
  [[nodiscard]] const std::string &status() const noexcept { return status_; }
  [[nodiscard]] std::vector<std::string> breadcrumbs() const;
  /// Layouts run so far: a settled frame runs none.
  [[nodiscard]] std::uint64_t layouts() const noexcept { return layouts_; }

  // -- gleditor::FrameContributor
  // -----------------------------------------------
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawScene(gleditor::FrameContext &ctx) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  /// While a layout change is still tweening (U6a).
  [[nodiscard]] bool busy() const override;

  // -- gleditor::PickObserver
  // ---------------------------------------------------
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // -- gleditor::a11y::Source
  // ---------------------------------------------------
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t nodeId, gleditor::a11y::Action action,
                     std::string_view value) override;

  // -- SlicePresentation
  // --------------------------------------------------------
  [[nodiscard]] gleditor::FrameContributor *
  frameContributor() noexcept override {
    return this;
  }
  [[nodiscard]] gleditor::PickObserver *pickObserver() noexcept override {
    return this;
  }
  [[nodiscard]] gleditor::a11y::Source *
  accessibilitySource() noexcept override {
    return this;
  }
  [[nodiscard]] gleditor::ui::FocusScope *focusScope() noexcept override {
    return this;
  }
  ViewSlicePresentation *setPresentationVisible(bool visible) override;
  [[nodiscard]] bool presentationVisible() const noexcept override {
    return visible_;
  }
  ViewSlicePresentation *setPresentationOrigin(glm::vec3 origin) override;
  ViewSlicePresentation *setPresentationTransformResolver(
      PresentationTransformResolver resolver) override;
  ViewSlicePresentation *
  setPresentationConfig(ZigzagPresentationConfig config) override;
  [[nodiscard]] const ZigzagPresentationConfig &
  presentationConfig() const noexcept override {
    return cells_;
  }
  void bindXuduStore(Store &store, const MicroversionId &version) override;
  [[nodiscard]] Store *store() const noexcept override { return store_; }
  [[nodiscard]] MicroversionId sliceHead() const override { return head_; }
  [[nodiscard]] const zigzag::ViewAxisBinding &currentView() const override;
  ViewSlicePresentation *
  setPreviewCell(std::optional<zigzag::CellRef> cell) override;
  ViewSlicePresentation *setExternInspector(ExternInspector inspector) override;
  ViewSlicePresentation *setHasKeyboard(bool has) noexcept override;
  ViewSlicePresentation *setKeyHints(std::string here,
                                     std::string elsewhere) override;
  /// Steps along a binding point ("step-x-pos", "std:nav/step_x_pos"), jumps
  /// home, hops to the head or tail of the x rank, swaps x and y, the
  /// sub-view commands and a typed binding ('std:view/bind x d.across': one
  /// argument may follow the name after a space) run on the owner thread;
  /// anything else is refused, said in the chrome and announced.
  bool dispatchAction(std::string_view actionName) override;
  bool
  insertConnectedTransclusion(std::span<const PrimediaSpan> spans) override;

  // -- ZigzagPresentationSurface
  // ------------------------------------------------
  [[nodiscard]] const zigzag::Manifold &manifold() const noexcept override;
  [[nodiscard]] zigzag::CellRef focusCell() const noexcept override;
  void focusCell(zigzag::CellRef cell) override;
  void activateCell(zigzag::CellRef cell) override;
  void setCellActivationCallback(CellActivationCallback callback) override {
    activation_ = std::move(callback);
  }
  [[nodiscard]] int cellRadius() const noexcept override { return cellRadius_; }
  void setCellRadius(int radius) noexcept override;
  /// Transcopyright locks are the legacy engine's; no cell is locked here.
  [[nodiscard]] bool isCellLocked(zigzag::CellRef cell) const noexcept override;
  [[nodiscard]] std::optional<TranscopyrightDescriptor>
  cellRoyalty(zigzag::CellRef cell) const noexcept override;
  bool unlockCell(zigzag::CellRef cell) override;
  [[nodiscard]] std::optional<CellAnchor>
  cellAnchor(zigzag::CellRef cell) const override;
  [[nodiscard]] std::optional<glm::vec3> focusCentre() const override;
  void setCellHighlights(std::vector<CellHighlight> highlights,
                         std::uint32_t borderColour) override;
  [[nodiscard]] std::uint64_t bridgeRevision() const noexcept override {
    return revision_;
  }
  void setBridgeInvalidationCallback(InvalidationCallback callback) override {
    invalidation_ = std::move(callback);
  }

private:
  void runOnOwner(std::function<void()> task);
  /// Fold the slice and bind its axes, if the store or version changed.
  void ensureSpace() const;
  void buildSpace();
  /// Open a slice that has no bindings of its own yet showing what its origin
  /// links along, or @p wanted where it names dimensions (view-system.md
  /// §7.3). Seeding happens once, when the slice is first folded; arriving
  /// somewhere never binds anything.
  void seedAxes(const std::vector<std::string> &wanted);
  /// Bind the slice as it was kept: carried over a re-fold, or read from
  /// system://layout. True when there was something to restore.
  bool restoreBindings(const std::optional<SavedBindings> &carried);
  [[nodiscard]] std::optional<SavedBindings> keptBindings() const;
  /// Arriving at a cell that links along something, with no spatial point
  /// showing anything, binds them as a slice does on opening and says so.
  void bindWhatIsLeftUnbound();
  void persistBindings();
  void announce(const ReplayReport &report);
  /// The name of the @p ordinal-th spatial binding point, in order: where the
  /// plane's steps, hops and swaps go, whatever the points are called.
  [[nodiscard]] std::optional<std::string>
  spatialPoint(std::size_t ordinal) const;
  void refreshStatus();
  void mintCues();
  void relayout(const PaneFrame &frame);
  [[nodiscard]] glm::mat4 placementToWorld() const;
  [[nodiscard]] float readableScale(const gleditor::FrameContext &ctx,
                                    const glm::mat4 &unscaled) const;
  [[nodiscard]] ZigzagPresentationConfig cellsInPixels() const;
  void step(const std::string &point, zigzag::DimVector direction);
  void hop(zigzag::DimVector direction);
  void jumpHome();
  void swapAxes();
  /// The bindings changed: toss what was derived, lay out again, and tell the
  /// compass and the legacy binding.
  void rebound(const MotionCause cause);
  void syncLegacyView();
  void refreshCompass();
  void persistSubview();
  void showSubview();
  void arrive(const MoveOutcome &outcome, bool recorded);
  void goTo(zigzag::CellRef cell, bool recorded);
  void record(zigzag::CellRef cell);
  void say(ViewMessage message, std::span<const MessageField> fields);
  void refused(std::string_view action);
  void changed();
  void refreshBreadcrumbs();
  [[nodiscard]] std::string_view textOf(SubjectId subject);
  [[nodiscard]] std::string shownName(ViewAxisId axis) const;
  [[nodiscard]] DimensionCue cueOf(std::uint64_t relation);

  ViewDescriptor descriptor_;
  std::unique_ptr<SliceView> view_;
  Host host_;
  SlicePresenter presenter_;
  std::atomic<std::thread::id> owner_{};

  Store *store_{};
  MicroversionId head_;
  mutable bool dirty_{};
  mutable std::unique_ptr<zigzag::Manifold> manifold_;
  mutable std::unique_ptr<ViewManifold> space_;
  /// The store the space was folded from, so a re-fold of another slice does
  /// not carry this one's bindings over.
  mutable const Store *builtFrom_{};
  /// The legacy visualizer's axes, by name, offered to a first fold.
  mutable std::vector<std::string> adopted_;
  bool focusDirty_{};
  /// When the focus last moved, and the pause that makes it a resting place
  /// (activity.settleMs): a walk is one record, not a record a step.
  std::chrono::steady_clock::time_point focusMovedAt_;
  std::uint32_t focusSettleMs_{WalkRankingConfig{}.settleMs};
  mutable std::optional<zigzag::CellRef> wantedFocus_;
  mutable zigzag::ViewAxisBinding legacyView_;
  SliceCursor cursor_{.origin = zigzag::noCell};
  LayoutSink sink_;
  /// What the animation says to draw this frame, and its stamp.
  ViewAnimation animation_;
  LayoutSink drawnSink_;
  std::uint64_t stamp_{1};
  MotionCause cause_{MotionCause::Move};
  bool retargeted_{};
  std::optional<std::chrono::steady_clock::time_point> lastFrame_;
  std::uint64_t layouts_{};
  bool stale_{true};
  std::optional<PaneFrame> laidOutAt_;
  std::uint64_t laidOutTypography_{};

  ZigzagPresentationConfig cells_;
  StretchConfig stretch_;
  DimensionCues cues_;
  CompassConfig compassConfig_;
  std::vector<CompassArm> compass_;
  std::uint64_t compassStamp_{1};
  std::uint32_t subview_{};
  std::vector<std::string> bindingPoints_;
  std::unordered_map<zigzag::CellRef, std::string> cellText_;
  std::unordered_map<std::uint64_t, std::string> labelText_;

  bool visible_{true};
  glm::vec3 origin_{0.0F};
  PresentationTransformResolver resolver_;
  glm::mat4 toWorld_{1.0F};
  float readableScale_{1.0F};
  int cellRadius_{3};
  std::vector<CellHighlight> highlights_;
  std::uint32_t highlightBorder_{};
  std::optional<zigzag::CellRef> preview_;
  ExternInspector externInspector_;
  CellActivationCallback activation_;
  InvalidationCallback invalidation_;
  std::uint64_t revision_{1};

  InMemoryActivityLog ownLog_;
  ActivityLog *log_{&ownLog_};
  std::vector<std::string> breadcrumbs_;
  std::string notice_, status_;
  std::uint64_t chromeRevision_{1};
  /// The chrome and compass stamps last drawn, so a change not yet drawn is
  /// work (busy()).
  std::uint64_t drawnChrome_{}, drawnCompass_{};
  std::string hintsHere_, hintsElsewhere_;
  /// The strip's rows under the message and the walk: the view and sub-view
  /// showing, on a row of their own so a message never covers them, then the
  /// hints. Composed when the chrome changes, not on every frame.
  std::string chromeRows_;
  std::uint64_t chromeRowsStamp_{~std::uint64_t{}};
  std::atomic<bool> keyboardHere_{true};
  std::atomic<std::uint64_t> keyboardMoves_{0};

  mutable std::mutex a11yLock_;
  std::unordered_map<SubjectId, std::uint64_t> a11yIds_;
  std::unordered_map<std::uint64_t, SubjectId> a11yTargets_;
  std::uint64_t nextA11yId_{};
};

} // namespace xanadu::view

#endif // COMMON_UI_VIEW_VIEW_SLICE_PRESENTATION_HPP

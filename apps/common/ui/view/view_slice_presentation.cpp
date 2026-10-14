/**
 * @file view_slice_presentation.cpp
 * @brief One placement of a slice view, behind the seam xuzz holds.
 */
#include "view_slice_presentation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <ranges>
#include <stdexcept>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/render_state.hpp>

#include "common/ui/readable_scale.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/view/view_binding.hpp"

namespace xanadu::view {

namespace {

constexpr const char *kCategory = "view.slice";

/// The accessibility tree's fixed nodes; placed things count up from
/// kFirstNode, each keeping its number for as long as its subject is drawn.
constexpr std::uint64_t kRootNode    = 1;
constexpr std::uint64_t kTrailNode   = 2;
constexpr std::uint64_t kNoticeNode  = 3;
constexpr std::uint64_t kCompassNode = 4;
constexpr std::uint64_t kStatusNode  = 5;
constexpr std::uint64_t kFirstNode   = 16;
/// The breadcrumbs' items, by place in the strip, clear of placed things.
constexpr std::uint64_t kFirstCrumbNode = std::uint64_t{1} << 40U;
/// The compass's arms, in order, in a range of their own.
constexpr std::uint64_t kFirstArmNode = std::uint64_t{1} << 41U;

/// The names a step arrives under: the bare-key action ZigZag's commands
/// dispatch ("step-x-pos"), and the keymap's own spellings
/// ("std:nav/step_x_pos", "std:nav/zigzag_step_x_pos").
struct StepAction {
  std::string point;
  zigzag::DimVector direction{zigzag::DimVector::POS};
};

[[nodiscard]] std::optional<StepAction> stepNamed(std::string_view action) {
  for (const auto prefix :
       {std::string_view{"std:nav/zigzag_step_"},
        std::string_view{"std:nav/step_"}, std::string_view{"step-"}}) {
    if (!action.starts_with(prefix)) {
      continue;
    }
    action.remove_prefix(prefix.size());
    const auto cut = action.find_last_of("-_");
    if (std::string_view::npos == cut || 0 == cut) {
      return std::nullopt;
    }
    const auto way = action.substr(cut + 1);
    if ("pos" != way && "neg" != way) {
      return std::nullopt;
    }
    return StepAction{.point     = std::string(action.substr(0, cut)),
                      .direction = "pos" == way ? zigzag::DimVector::POS
                                                : zigzag::DimVector::NEG};
  }
  return std::nullopt;
}

[[nodiscard]] bool isJumpHome(const std::string_view action) {
  return "jump-home" == action || "std:nav/jump_home" == action ||
         "std:nav/zigzag_jump_home" == action;
}

/// Hopping to the head or the tail of the x rank, under the bare-key name and
/// the keymap's.
[[nodiscard]] std::optional<zigzag::DimVector>
hopNamed(const std::string_view action) {
  if ("hop-head" == action || "std:nav/hop_head" == action ||
      "std:nav/zigzag_hop_head" == action) {
    return zigzag::DimVector::NEG;
  }
  if ("hop-tail" == action || "std:nav/hop_tail" == action ||
      "std:nav/zigzag_hop_tail" == action) {
    return zigzag::DimVector::POS;
  }
  return std::nullopt;
}

[[nodiscard]] bool isSwapAxes(const std::string_view action) {
  return "swap-xy" == action || "std:ui/swap_axes" == action ||
         "std:ui/zigzag_swap_xy" == action;
}

/// A call as the keymap and a typed command spell it: the action's name, then
/// optionally one argument after a space.
struct Call {
  std::string_view name, argument;
};

[[nodiscard]] Call callOf(const std::string_view action) {
  const auto cut = action.find(' ');
  if (std::string_view::npos == cut) {
    return {.name = action, .argument = {}};
  }
  auto argument = action.substr(cut + 1);
  while (!argument.empty() && ' ' == argument.front()) {
    argument.remove_prefix(1);
  }
  return {.name = action.substr(0, cut), .argument = argument};
}

/// An action as a reader would say it: "toggle palette" for
/// "std:ui/toggle_palette" or "toggle-palette".
[[nodiscard]] std::string spokenAction(std::string_view action) {
  if (const auto slash = action.find('/'); std::string_view::npos != slash) {
    action.remove_prefix(slash + 1);
  }
  std::string out(action);
  std::ranges::replace(out, '_', ' ');
  std::ranges::replace(out, '-', ' ');
  return out;
}

/// A placement standing on its own at @p origin: one layout unit is one
/// canvas pixel, which the world measures as a document's pixel does, so a
/// cell's text is the size of a page's.
[[nodiscard]] glm::mat4 originTransform(const glm::vec3 origin) {
  return glm::translate(glm::mat4{1.0F}, origin) *
         glm::scale(glm::mat4{1.0F}, glm::vec3{Doc::pixelsToWorld});
}

[[nodiscard]] bool sameFrame(const PaneFrame &a, const PaneFrame &b) noexcept {
  return a.widthPx == b.widthPx && a.heightPx == b.heightPx &&
         a.localToClip == b.localToClip &&
         a.minReadableLinePx == b.minReadableLinePx;
}

/// The cells of the slice a view is never centred on: the slice's own list
/// of dimensions and its settings' plumbing are reached through them.
[[nodiscard]] bool bindable(const std::string &name) {
  return !name.empty() && name != kDimDims;
}

} // namespace

ViewSlicePresentation::ViewSlicePresentation(const ViewRegistry &registry,
                                             const std::string_view kind,
                                             Host host,
                                             std::string fontOverride)
    : host_(std::move(host)), presenter_(std::move(fontOverride)) {
  const auto found = registry.find(kind);
  if (!found || ViewSubject::Slice != found->subject || !found->make) {
    throw std::invalid_argument(
        std::format("no slice view of kind '{}' is installed", kind));
  }
  descriptor_ = *found;
  auto made   = descriptor_.make();
  view_.reset(dynamic_cast<SliceView *>(made.get()));
  if (!view_) {
    throw std::invalid_argument(
        std::format("the view of kind '{}' is not a slice view", kind));
  }
  std::ignore = made.release();
  toWorld_    = originTransform(origin_);

  // The host seeds a descriptor's settings where the built-in ones are
  // (view-system.md §8.1), so the reader finds them in system://settings.
  if (host_.systemStore) {
    if (auto *const settings = host_.systemStore(SystemDocKind::Settings)) {
      const auto before = settings->primaryCurrentVersion();
      auto head         = before;
      for (const auto &spec : descriptor_.settings) {
        head = ensureSetting(*settings, head, spec);
      }
      if (head != before && host_.repoint) {
        host_.repoint(SystemDocKind::Settings, head);
      }
    }
  }
  applySettings();
}

ViewSlicePresentation::~ViewSlicePresentation() {
  // The space reads the manifold it was made over.
  space_.reset();
  manifold_.reset();
}

// -- settings -----------------------------------------------------------------

ViewSlicePresentation *ViewSlicePresentation::applySettings() {
  const auto store = [this](const SystemDocKind kind) -> Store * {
    return host_.systemStore ? host_.systemStore(kind) : nullptr;
  };
  if (auto *const settings = store(SystemDocKind::Settings)) {
    const auto model = SystemStoreModel::fromStore(*settings);
    stretch_         = StretchConfig::fromSettings(model);
    view_->applySettings(model, cellsInPixels());
  }
  if (auto *const layout = store(SystemDocKind::Layout)) {
    const auto model = SystemStoreModel::fromStore(*layout);
    std::vector<std::string> points;
    for (const auto &point :
         bindingPointsFrom(model.getValues(settings::kBindingPoints))) {
      points.push_back(point.name + ":" + std::string(point.role.get().id()));
    }
    if (points != bindingPoints_) {
      bindingPoints_ = std::move(points);
      dirty_         = nullptr != store_;
    }
    if (nullptr != store_) {
      const auto id =
          model.getString(settings::sliceSubviewKey(store_->documentId().str(),
                                                    descriptor_.kind),
                          std::string{});
      if (const auto found = subviewIndexOf(descriptor_, id);
          found && *found != subview_) {
        subview_ = *found;
        cause_   = MotionCause::SubViewSwitch;
      }
    }
  }
  if (auto *const ui = store(SystemDocKind::UI)) {
    cues_ = DimensionCues::fromSettings(SystemStoreModel::fromStore(*ui));
    if (const auto compass = CompassConfig::fromStore(*ui);
        compass != compassConfig_) {
      compassConfig_ = compass;
      ++compassStamp_;
      ++chromeRevision_;
    }
    animation_.configure(MotionConfig::fromStore(*ui));
    presenter_.setDrawConfig(ViewDrawConfig::fromStore(*ui));
  }
  refreshCompass();
  refreshStatus();
  stale_ = true;
  ++stamp_;
  return this;
}

ZigzagPresentationConfig ViewSlicePresentation::cellsInPixels() const {
  // The layout's unit is a canvas pixel (§8.4); the zigzag.* lengths are
  // logical, so ui.scale reaches the padding as it reaches the text.
  const auto &metrics           = presenter_.metrics();
  auto cells                    = cells_;
  cells.cellHorizontalPaddingPx = metrics.px(cells_.cellHorizontalPaddingPx);
  cells.cellVerticalPaddingPx   = metrics.px(cells_.cellVerticalPaddingPx);
  cells.contentMaxWidthPx       = metrics.px(cells_.contentMaxWidthPx);
  return cells;
}

ViewSlicePresentation *
ViewSlicePresentation::adoptAxes(const zigzag::ViewAxisBinding &names) {
  legacyView_ = names;
  adopted_    = {names.x_dimension, names.y_dimension, names.z_dimension};
  dirty_      = nullptr != store_;
  return this;
}

ViewSlicePresentation *ViewSlicePresentation::setActivityLog(ActivityLog *log) {
  log_ = nullptr != log ? log : &ownLog_;
  refreshBreadcrumbs();
  return this;
}

// -- the slice ----------------------------------------------------------------

void ViewSlicePresentation::bindXuduStore(Store &store,
                                          const MicroversionId &version) {
  if (cursor_.origin != zigzag::noCell && &store == store_) {
    wantedFocus_ = cursor_.origin;
  } else if (&store != store_) {
    wantedFocus_.reset();
  }
  if (&store != store_) {
    subview_ = 0;
  }
  store_ = &store;
  head_  = version;
  if (head_.isZero()) {
    head_ = store.primaryCurrentVersion();
  }
  if (head_.isZero()) {
    head_ = store.latest();
  }
  // Folded when first needed: a presentation standing behind another costs
  // nothing until it is shown.
  dirty_ = true;
  applySettings();
  changed();
}

void ViewSlicePresentation::ensureSpace() const {
  if (dirty_) {
    dirty_ = false;
    // Folding is the owner's; a const read that finds it out of date does it
    // once rather than answer from a slice that is not the one bound.
    const_cast<ViewSlicePresentation *>(this)->buildSpace();
  }
}

void ViewSlicePresentation::buildSpace() {
  // What the reader has set is carried across a re-fold of the same slice by
  // name, groups included; a different slice starts from what was kept for it.
  std::optional<SavedBindings> carried;
  if (space_ && nullptr != store_ && builtFrom_ == store_) {
    carried = saveBindings(*space_, *store_);
    if (cursor_.origin != zigzag::noCell && !wantedFocus_) {
      wantedFocus_ = cursor_.origin;
    }
  }
  space_.reset();
  manifold_.reset();
  builtFrom_ = nullptr;
  cellText_.clear();
  labelText_.clear();
  if (nullptr == store_) {
    refreshCompass();
    return;
  }
  manifold_ =
      std::make_unique<zigzag::Manifold>(store_->rebuildManifold(head_));
  space_     = std::make_unique<ViewManifold>(*manifold_);
  builtFrom_ = store_;

  std::vector<BindingPoint> points;
  if (host_.systemStore) {
    if (auto *const layout = host_.systemStore(SystemDocKind::Layout)) {
      points = bindingPointsFrom(SystemStoreModel::fromStore(*layout).getValues(
          settings::kBindingPoints));
    }
  }
  if (points.empty()) {
    // No layout to ask: the points the setting itself defaults to.
    for (const auto &spec : defaultSettingSpecs(SystemDocKind::Layout)) {
      if (settings::kBindingPoints == spec.name && !spec.schemas.empty()) {
        points = bindingPointsFrom(spec.schemas.front().defaultValues);
      }
    }
  }
  if (!space_->axes().configure(points)) {
    GLEDITOR_LOG_WARN(kCategory, "the binding points could not be configured");
  }

  auto origin = wantedFocus_.value_or(manifold_->home());
  if (!carried && !wantedFocus_ && host_.systemStore) {
    if (auto *const layout = host_.systemStore(SystemDocKind::Layout)) {
      const auto kept = SystemStoreModel::fromStore(*layout).getInt64List(
          settings::sliceFocusKey(store_->documentId().str()));
      if (!kept.empty() && kept.front() >= 0) {
        origin = static_cast<zigzag::CellRef>(kept.front());
      }
    }
  }
  if (!manifold_->contains(origin)) {
    origin = manifold_->home();
  }
  if (!manifold_->contains(origin) && manifold_->cellCount() > 0) {
    origin = manifold_->cells().front().birthOp;
  }
  wantedFocus_.reset();
  cursor_ = SliceCursor{.origin = origin};
  if (!restoreBindings(carried)) {
    seedAxes(adopted_);
  }
  syncLegacyView();
  mintCues();
  refreshCompass();
  cause_ = MotionCause::Rebind;
  stale_ = true;
}

std::optional<SavedBindings> ViewSlicePresentation::keptBindings() const {
  if (nullptr == store_ || !host_.systemStore) {
    return std::nullopt;
  }
  const auto *const layout = host_.systemStore(SystemDocKind::Layout);
  if (nullptr == layout) {
    return std::nullopt;
  }
  const auto model = SystemStoreModel::fromStore(*layout);
  const auto id    = store_->documentId().str();
  SavedBindingCells cells{
      .axes      = model.getStringList(settings::sliceAxesKey(id)),
      .groups    = model.getStringList(settings::sliceGroupsKey(id)),
      .ringOrder = model.getStringList(settings::sliceRingOrderKey(id))};
  // Nothing is kept for a slice whose reader has not bound anything: the
  // first thing saved is the axes, which are never empty once there are any.
  if (cells.axes.empty()) {
    return std::nullopt;
  }
  auto saved = bindingsFromCells(cells);
  if (!saved) {
    GLEDITOR_LOG_WARN(kCategory,
                      "the bindings kept for slice {} are not in a shape this "
                      "program writes; ignored",
                      id);
  }
  return saved;
}

bool ViewSlicePresentation::restoreBindings(
    const std::optional<SavedBindings> &carried) {
  const auto saved = carried ? carried : keptBindings();
  if (!saved) {
    return false;
  }
  // A restore that loses nothing says nothing; a name that has gone is said.
  announce(replayBindings(*space_, *saved, *store_));
  return true;
}

void ViewSlicePresentation::announce(const ReplayReport &report) {
  if (report.unresolved.empty()) {
    return;
  }
  const auto missing  = std::to_string(report.unresolved.size());
  const auto restored = std::to_string(report.restored);
  const std::array summary{MessageField{"restored", restored},
                           MessageField{"missing", missing}};
  std::string words = spokenMessage(report.summary, summary);
  for (const auto &notice : report.unresolved) {
    // Each notice names its place under the field its words use.
    const std::array fields{MessageField{"name", notice.name},
                            MessageField{"axis", notice.where},
                            MessageField{"group", notice.where},
                            MessageField{"where", notice.where},
                            MessageField{"inner", notice.name},
                            MessageField{"outer", notice.where}};
    words += " " + spokenMessage(notice.message, fields);
  }
  notice_ = std::move(words);
  ++chromeRevision_;
  ++revision_;
}

std::optional<std::string>
ViewSlicePresentation::spatialPoint(const std::size_t ordinal) const {
  if (!space_) {
    return std::nullopt;
  }
  std::size_t seen = 0;
  for (ViewAxisId axis = 0; axis < space_->axes().axisCount(); ++axis) {
    const auto role = space_->axes().role(axis);
    if (!role || !role->spatial()) {
      continue;
    }
    if (seen++ == ordinal) {
      return space_->axes().pointName(axis);
    }
  }
  return std::nullopt;
}

void ViewSlicePresentation::seedAxes(const std::vector<std::string> &wanted) {
  auto &axes = space_->axes();
  const auto exists =
      [this](const std::string &name) -> std::optional<zigzag::DimRef> {
    if (!bindable(name)) {
      return std::nullopt;
    }
    return manifold_->dimensionNamed(name, *store_);
  };
  // What the origin links along, in the slice's own order: a slice opens
  // showing what it uses, as the legacy visualizer fits its view to home.
  std::vector<zigzag::DimRef> used;
  if (manifold_->contains(cursor_.origin)) {
    for (const auto &link : manifold_->dimensionsOf(cursor_.origin)) {
      if ((link.pos != zigzag::noCell || link.neg != zigzag::noCell) &&
          bindable(manifold_->textOf(link.dim, *store_)) &&
          !std::ranges::contains(used, link.dim)) {
        used.push_back(link.dim);
      }
    }
  }
  std::vector<zigzag::DimRef> chosen;
  std::size_t ordinal = 0;
  for (ViewAxisId axis = 0; axis < axes.axisCount(); ++axis) {
    const auto role = axes.role(axis);
    if (!role || !role->spatial()) {
      continue;
    }
    std::optional<zigzag::DimRef> dim;
    if (ordinal < wanted.size()) {
      dim = exists(wanted[ordinal]);
    }
    ++ordinal;
    if (!dim) {
      const auto next = std::ranges::find_if(used, [&](const auto candidate) {
        return !std::ranges::contains(chosen, candidate);
      });
      if (next != used.end()) {
        dim = *next;
      }
    }
    if (dim && axes.bind(axis, *dim)) {
      chosen.push_back(*dim);
    }
  }
  axes.forgetHistory();
}

void ViewSlicePresentation::syncLegacyView() {
  // The legacy binding, for the satelloid and for a switch back: its three
  // fields are the plane and depth, so they take the spatial points in order.
  std::array<std::string *, 3> legacy{&legacyView_.x_dimension,
                                      &legacyView_.y_dimension,
                                      &legacyView_.z_dimension};
  if (!space_) {
    return;
  }
  std::size_t next = 0;
  for (ViewAxisId axis = 0; axis < space_->axes().axisCount(); ++axis) {
    const auto role = space_->axes().role(axis);
    if (!role || !role->spatial()) {
      continue;
    }
    if (next == legacy.size()) {
      break;
    }
    if (auto name = shownName(axis); !name.empty()) {
      *legacy[next] = std::move(name);
    }
    ++next;
  }
}

void ViewSlicePresentation::refreshCompass() {
  std::vector<CompassArm> arms;
  if (space_ && nullptr != store_) {
    arms =
        compassOf(*space_, *store_, [this](const std::string_view dimension) {
          return cues_.cueOf(dimension).value_or(
              DimensionPalette::standard().cueOf(cues_.size()));
        });
  }
  if (arms != compass_) {
    compass_ = std::move(arms);
    ++compassStamp_;
    ++chromeRevision_;
  }
}

void ViewSlicePresentation::mintCues() {
  if (!space_ || !host_.systemStore) {
    return;
  }
  std::vector<std::string> unseen;
  for (ViewAxisId axis = 0; axis < space_->axes().axisCount(); ++axis) {
    const auto shown = space_->axes().shown(axis);
    if (!shown) {
      continue;
    }
    space_->axes().forEachLeaf(*shown, [&](const zigzag::DimRef dim) {
      auto name = manifold_->textOf(dim, *store_);
      if (!name.empty() && !cues_.cueOf(name) &&
          !std::ranges::contains(unseen, name)) {
        unseen.push_back(std::move(name));
      }
    });
  }
  if (unseen.empty()) {
    return;
  }
  // A cue is minted on first sight (plan §5.2). The write goes to the owner
  // between frames, never from inside a draw.
  runOnOwner([this, unseen = std::move(unseen)] {
    auto *const ui = host_.systemStore(SystemDocKind::UI);
    if (nullptr == ui) {
      return;
    }
    const auto before = ui->primaryCurrentVersion();
    auto head         = before;
    for (const auto &name : unseen) {
      head = assignDimensionCue(*ui, head, name);
    }
    if (head != before && host_.repoint) {
      host_.repoint(SystemDocKind::UI, head);
    }
    cues_ = DimensionCues::fromSettings(SystemStoreModel::fromStore(*ui));
    refreshCompass();
    ++stamp_;
  });
}

const zigzag::Manifold &ViewSlicePresentation::manifold() const noexcept {
  ensureSpace();
  static const zigzag::Manifold empty;
  return manifold_ ? *manifold_ : empty;
}

const zigzag::ViewAxisBinding &ViewSlicePresentation::currentView() const {
  ensureSpace();
  return legacyView_;
}

std::string ViewSlicePresentation::shownName(const ViewAxisId axis) const {
  if (!space_ || nullptr == store_) {
    return {};
  }
  const auto shown = space_->axes().shown(axis);
  if (!shown) {
    return {};
  }
  if (space_->axes().isGroup(*shown)) {
    return space_->axes().groupName(*shown).value_or(std::string{});
  }
  return manifold_->textOf(*shown, *store_);
}

std::string_view ViewSlicePresentation::textOf(const SubjectId subject) {
  if (!manifold_ || nullptr == store_) {
    return {};
  }
  switch (subject.kind) {
  case SubjectKind::Cell:
  case SubjectKind::Marker: {
    const auto cell    = static_cast<zigzag::CellRef>(subject.value);
    auto [found, made] = cellText_.try_emplace(cell);
    if (made && manifold_->contains(cell)) {
      found->second = manifold_->textOf(cell, *store_);
    }
    return found->second;
  }
  case SubjectKind::Label: {
    auto [found, made] = labelText_.try_emplace(subject.value);
    if (made && space_) {
      const auto target = static_cast<BindTarget>(subject.value);
      found->second     = space_->axes().isGroup(target)
                              ? space_->axes().groupName(target).value_or("")
                              : manifold_->textOf(target, *store_);
    }
    return found->second;
  }
  case SubjectKind::ViewCell:
  case SubjectKind::Page:
  case SubjectKind::Document:
  case SubjectKind::Badge:
    return {};
  }
  return {};
}

DimensionCue ViewSlicePresentation::cueOf(const std::uint64_t relation) {
  // A view, not a copy: this is asked for every edge on every frame a move
  // is animated, and a string made here is an allocation each time.
  const auto name = textOf(SubjectId::label(relation));
  if (const auto cue = cues_.cueOf(name)) {
    return *cue;
  }
  // Not yet recorded: the rule's next assignment, which mintCues() is about
  // to make permanent.
  return DimensionPalette::standard().cueOf(cues_.size());
}

// -- the pipeline
// ---------------------------------------------------------------

void ViewSlicePresentation::deviceReady(
    render::RenderDevice &device,
    const render::PipelineDesc &documentPipeline) {
  owner_ = std::this_thread::get_id();
  presenter_.deviceReady(device, documentPipeline);
  stale_ = true;
}

glm::mat4 ViewSlicePresentation::placementToWorld() const {
  if (resolver_) {
    if (const auto transform = resolver_()) {
      return *transform;
    }
  }
  return originTransform(origin_);
}

float ViewSlicePresentation::readableScale(const gleditor::FrameContext &ctx,
                                           const glm::mat4 &unscaled) const {
  // At the scale a page is drawn at, cell text lands smaller than it can be
  // read, embedded beside a page or not: the placement is enlarged until a
  // line reaches zigzag.minReadableTextPx, in quarter steps, so its cells
  // show their text rather than all stepping down to bars (§15).
  const float wanted = cells_.minReadableTextPx;
  const float line   = presenter_.bodyLineHeight();
  if (wanted <= 0.0F || line <= 0.0F || ctx.screenHeight <= 0) {
    return 1.0F;
  }
  const auto clip = [&](const glm::vec3 &at) {
    return ctx.viewProjection * unscaled * glm::vec4(at, 1.0F);
  };
  const auto top    = clip({0.0F, line, 0.0F});
  const auto bottom = clip({0.0F, 0.0F, 0.0F});
  if (top.w <= 0.0F || bottom.w <= 0.0F) {
    return readableScale_;
  }
  const float onScreen = std::abs((top.y / top.w) - (bottom.y / bottom.w)) *
                         0.5F * static_cast<float>(ctx.screenHeight);
  return onScreen > 0.0F ? xanadu::ui::readableScaleFor(onScreen, wanted)
                         : readableScale_;
}

void ViewSlicePresentation::relayout(const PaneFrame &frame) {
  ensureSpace();
  ++layouts_;
  GLEDITOR_LOG_DEBUG(kCategory,
                     "layout {} at {}x{}: placement scale {}, readable scale "
                     "{}, embedded {}",
                     layouts_, frame.widthPx, frame.heightPx,
                     glm::length(glm::vec3(toWorld_[0])), readableScale_,
                     static_cast<bool>(resolver_));
  sink_.clear();
  laidOutAt_         = frame;
  laidOutTypography_ = presenter_.typographyStamp();
  stale_             = false;
  if (space_ && manifold_->contains(cursor_.origin)) {
    const auto measure = [this](const SubjectId subject, const float width) {
      return presenter_.measure(subject, width, textOf(subject));
    };
    if (const auto prepared = view_->prepare(*space_, cursor_, frame, measure);
        !prepared) {
      say(messageKey(prepared.error()), {});
    } else {
      view_->layout(SliceLayoutInput{.space   = *space_,
                                     .cursor  = cursor_,
                                     .frame   = frame,
                                     .measure = measure,
                                     .subview = subview_},
                    sink_);
    }
  }
  animation_.retarget(AnimationTarget{
      .layout = sink_, .epoch = space_ ? space_->epoch() : 0, .cause = cause_});
  cause_      = MotionCause::Move;
  retargeted_ = true;
  ++revision_;
  if (invalidation_) {
    invalidation_(revision_);
  }
}

void ViewSlicePresentation::drawScene(gleditor::FrameContext &ctx) {
  owner_ = std::this_thread::get_id();
  if (!visible_) {
    lastFrame_.reset();
    return;
  }
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  if (presenter_.setTypography(metrics, ctx.theme)) {
    // Padding is in pixels, so it follows the scale the text just took.
    applySettings();
  }
  ensureSpace();

  const auto unscaled = placementToWorld();
  if (const auto scale = readableScale(ctx, unscaled);
      scale != readableScale_) {
    readableScale_ = scale;
  }
  toWorld_ = unscaled * glm::scale(glm::mat4{1.0F}, glm::vec3{readableScale_});
  const PaneFrame frame{
      .widthPx     = static_cast<float>(ctx.screenWidth),
      .heightPx    = static_cast<float>(ctx.screenHeight),
      .localToClip = ctx.viewProjection * toWorld_,
      .minReadableLinePx =
          std::max(cells_.minReadableTextPx, ctx.theme.type.minFontPx)};
  if (stale_ || !laidOutAt_ || !sameFrame(*laidOutAt_, frame) ||
      laidOutTypography_ != presenter_.typographyStamp()) {
    relayout(frame);
  }

  const auto now = std::chrono::steady_clock::now();
  const auto dt  = lastFrame_ ? ViewAnimation::Millis{now - *lastFrame_}
                              : ViewAnimation::Millis{0.0};
  lastFrame_     = now;
  if (retargeted_ || animation_.animating()) {
    animation_.advance(dt, drawnSink_);
    retargeted_ = false;
    ++stamp_;
  }

  // Written once the walk comes to rest, so a held key is one write.
  if (focusDirty_ && !animation_.animating()) {
    persistFocus();
  }

  const auto documentId = store_ ? store_->documentId().str() : std::string{};
  const auto version    = head_.str();
  const auto px         = cellsInPixels();
  presenter_.drawScene(
      ctx,
      PresentedPlacement{
          .records         = drawnSink_,
          .stamp           = stamp_,
          .toWorld         = toWorld_,
          .documentId      = documentId,
          .version         = version,
          .paddingX        = px.cellHorizontalPaddingPx,
          .paddingY        = px.cellVerticalPaddingPx,
          .contentMaxWidth = px.contentMaxWidthPx,
          .edgeWidth    = presenter_.metrics().px(cells_.connectionBeamWidthPx),
          .clipMarginPx = stretch_.clipMargin,
          .ghostOpacity = stretch_.ghostOpacity(),
          .highlights   = highlights_,
          .highlightBorder = highlightBorder_},
      PresentedWords{
          .text = [this](const SubjectId subject) { return textOf(subject); },
          .cue =
              [this](const std::uint64_t relation) {
                return cueOf(relation);
              }});
}

void ViewSlicePresentation::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible_) {
    return;
  }
  presenter_.drawChrome(
      ctx, PresentedChrome{.breadcrumbs = breadcrumbs_,
                           .notice      = notice_.empty() ? status_ : notice_,
                           .hints       = keyboardHere_.load() ? hintsHere_
                                                               : hintsElsewhere_,
                           .paddingX    = cells_.hudHorizontalPaddingPx,
                           .paddingY    = cells_.hudVerticalPaddingPx,
                           .stamp = chromeRevision_ + keyboardMoves_.load()});
  ensureSpace();
  presenter_.drawCompass(ctx, PresentedCompass{.arms   = compass_,
                                               .config = compassConfig_,
                                               .stamp  = compassStamp_});
}

bool ViewSlicePresentation::busy() const {
  return visible_ && animation_.animating();
}

// -- moving -------------------------------------------------------------------

void ViewSlicePresentation::runOnOwner(std::function<void()> task) {
  const auto owner = owner_.load();
  if (host_.post && owner != std::thread::id{} &&
      owner != std::this_thread::get_id()) {
    host_.post(std::move(task));
  } else {
    task();
  }
}

bool ViewSlicePresentation::dispatchAction(const std::string_view actionName) {
  const auto [name, argument] = callOf(actionName);
  if (auto stepping = stepNamed(name)) {
    runOnOwner([this, stepping = std::move(*stepping)] {
      step(stepping.point, stepping.direction);
    });
    return true;
  }
  if (isJumpHome(name)) {
    runOnOwner([this] { jumpHome(); });
    return true;
  }
  if (const auto hopping = hopNamed(name)) {
    runOnOwner([this, direction = *hopping] { hop(direction); });
    return true;
  }
  if (isSwapAxes(name)) {
    runOnOwner([this] { swapAxes(); });
    return true;
  }
  if (settings::kKeymapViewSubviewNext == name ||
      settings::kKeymapViewSubviewPrev == name) {
    runOnOwner([this, by = settings::kKeymapViewSubviewNext == name ? 1 : -1] {
      cycleSubview(by);
    });
    return true;
  }
  if (settings::kKeymapViewBind == name) {
    runOnOwner([this, typed = std::string(argument)] { bindTyped(typed); });
    return true;
  }
  runOnOwner([this, action = std::string(name)] { refused(action); });
  return false;
}

void ViewSlicePresentation::refused(const std::string_view action) {
  const auto spoken = spokenAction(action);
  const std::array fields{MessageField{"view", descriptor_.name},
                          MessageField{"action", spoken}};
  say(ViewMessage::ActionNotSupported, fields);
  GLEDITOR_LOG_DEBUG(kCategory, "{} cannot run {} yet", descriptor_.kind,
                     action);
}

void ViewSlicePresentation::step(const std::string &point,
                                 const zigzag::DimVector direction) {
  ensureSpace();
  if (!space_) {
    return;
  }
  const auto axis = bindingPointNamed(space_->axes(), point);
  if (!axis) {
    const std::array fields{MessageField{"axis", point}};
    say(ViewMessage::AxisUnknown, fields);
    return;
  }
  if (!space_->axes().shown(*axis)) {
    // Not a dead end of the slice but of the binding: said as what to do.
    const auto name = space_->axes().pointName(*axis).value_or(point);
    const std::array fields{MessageField{"axis", name}};
    say(ViewMessage::PointShowsNothing, fields);
    return;
  }
  const auto outcome = view_->move(*space_, cursor_,
                                   MoveRequest{.kind      = MoveKind::AlongAxis,
                                               .axis      = axis,
                                               .direction = direction});
  if (!outcome.moved) {
    const auto name = shownName(*axis);
    const std::array fields{
        MessageField{"dimension", name.empty() ? point : name},
        MessageField{"direction", zigzag::DimVector::POS == direction
                                      ? "posward"
                                      : "negward"}};
    say(ViewMessage::NothingThatWay, fields);
    return;
  }
  arrive(outcome, true);
}

void ViewSlicePresentation::hop(const zigzag::DimVector direction) {
  ensureSpace();
  if (!space_) {
    return;
  }
  // Along the first point of the plane, whatever the points are called.
  const auto point = spatialPoint(0);
  const auto axis  = point ? space_->axes().axisNamed(*point) : std::nullopt;
  if (!axis) {
    const std::array fields{MessageField{"axis", "of the plane"}};
    say(ViewMessage::AxisUnknown, fields);
    return;
  }
  // A rank is a path or a ring (a cell has one neighbour each way), so the
  // walk ends at the rank's end or on coming back to where it began; in a
  // ring the head and the tail are the cells either side of the start, and a
  // hop from the start goes round to the one behind it. The step count is
  // the rank's length, not the slice's.
  const auto start = cursor_.origin;
  auto at          = cursor_;
  bool moved       = false;
  bool origin      = false;
  for (;;) {
    const auto outcome = view_->move(*space_, at,
                                     MoveRequest{.kind = MoveKind::AlongAxis,
                                                 .axis = axis,
                                                 .direction = direction});
    if (!outcome.moved || outcome.cursor == at ||
        outcome.cursor.origin == start) {
      break;
    }
    at     = outcome.cursor;
    moved  = true;
    origin = origin || outcome.originChanged;
  }
  if (!moved) {
    const auto name = shownName(*axis);
    const std::array fields{
        MessageField{"dimension", name.empty() ? *point : name},
        MessageField{"direction", zigzag::DimVector::POS == direction
                                      ? "posward"
                                      : "negward"}};
    say(ViewMessage::NothingThatWay, fields);
    return;
  }
  arrive(MoveOutcome{.cursor = at, .moved = true, .originChanged = origin},
         true);
}

void ViewSlicePresentation::jumpHome() {
  ensureSpace();
  if (manifold_ && manifold_->contains(manifold_->home())) {
    goTo(manifold_->home(), true);
  }
}

void ViewSlicePresentation::swapAxes() {
  ensureSpace();
  if (!space_) {
    return;
  }
  // The first two points of the plane, whatever the points are called.
  auto &axes        = space_->axes();
  const auto first  = spatialPoint(0);
  const auto second = spatialPoint(1);
  const auto x      = first ? axes.axisNamed(*first) : std::nullopt;
  const auto y      = second ? axes.axisNamed(*second) : std::nullopt;
  if (!x || !y) {
    const std::array fields{MessageField{"axis", "of the plane"}};
    say(ViewMessage::AxisUnknown, fields);
    return;
  }
  if (const auto swapped = axes.swap(*x, *y); !swapped) {
    say(messageKey(swapped.error()), {});
    return;
  }
  rebound(MotionCause::Rebind);
  persistBindings();
}

void ViewSlicePresentation::rebound(const MotionCause cause) {
  space_->toss();
  syncLegacyView();
  mintCues();
  refreshCompass();
  cause_ = cause;
  stale_ = true;
  changed();
}

bool ViewSlicePresentation::bindTyped(const std::string_view command) {
  ensureSpace();
  if (!space_ || nullptr == store_) {
    return false;
  }
  const auto parsed = parseBindCommand(command);
  if (!parsed) {
    say(ViewMessage::CommandUnclear, {});
    return false;
  }
  const auto bound = bindByName(*space_, *store_, *parsed);
  if (!bound) {
    switch (bound.error()) {
    case ViewError::UnknownAxis: {
      const std::array fields{MessageField{"axis", parsed->point}};
      say(ViewMessage::AxisUnknown, fields);
      break;
    }
    case ViewError::UnknownName: {
      const std::array fields{MessageField{"name", parsed->target}};
      say(ViewMessage::NameUnknown, fields);
      break;
    }
    case ViewError::EmptyGroupBind: {
      const std::array fields{MessageField{"name", parsed->target}};
      say(ViewMessage::EmptyGroup, fields);
      break;
    }
    default:
      say(messageKey(bound.error()), {});
      break;
    }
    return false;
  }
  rebound(MotionCause::Rebind);
  persistBindings();
  // One dimension on two points is allowed (§7.3): a step on either moves
  // along it. Saying so keeps the reader who did it by accident informed.
  std::string others;
  if (const auto shown = space_->axes().shown(bound->axis)) {
    space_->axes().forEachAxisShowing(*shown, [&](const ViewAxisId other) {
      if (other != bound->axis) {
        others += (others.empty() ? "" : ", ") +
                  space_->axes().pointName(other).value_or("?");
      }
    });
  }
  if (others.empty()) {
    const std::array fields{MessageField{"target", bound->shows},
                            MessageField{"axis", bound->point}};
    say(ViewMessage::AxisShows, fields);
  } else {
    const std::array fields{MessageField{"target", bound->shows},
                            MessageField{"axis", bound->point},
                            MessageField{"other", others}};
    say(ViewMessage::AxisAlsoShown, fields);
  }
  return true;
}

BindingChoices ViewSlicePresentation::bindingChoices() const {
  ensureSpace();
  if (!space_ || nullptr == store_) {
    return {};
  }
  return bindingChoicesOf(*space_, *store_);
}

std::string_view ViewSlicePresentation::subviewId() const noexcept {
  return subview_ < descriptor_.subviews.size()
             ? std::string_view{descriptor_.subviews[subview_].id}
             : std::string_view{};
}

ViewSlicePresentation *ViewSlicePresentation::cycleSubview(const int by) {
  const auto next = steppedSubview(descriptor_.subviews.size(), subview_, by);
  if (!next) {
    const std::array fields{MessageField{"view", descriptor_.name}};
    say(ViewMessage::NoSubviews, fields);
    return this;
  }
  subview_ = *next;
  showSubview();
  persistSubview();
  return this;
}

void ViewSlicePresentation::showSubview() {
  const auto place = std::to_string(subview_ + 1);
  const auto count = std::to_string(descriptor_.subviews.size());
  const std::array fields{
      MessageField{"view", descriptor_.name},
      MessageField{"subview", descriptor_.subviews[subview_].name},
      MessageField{"place", place}, MessageField{"count", count}};
  say(ViewMessage::SubviewShown, fields);
  refreshStatus();
  cause_ = MotionCause::SubViewSwitch;
  stale_ = true;
  changed();
}

void ViewSlicePresentation::persistSubview() {
  if (nullptr == store_ || !host_.systemStore) {
    return;
  }
  auto *const layout = host_.systemStore(SystemDocKind::Layout);
  if (nullptr == layout) {
    return;
  }
  const auto id     = store_->documentId().str();
  const auto before = layout->primaryCurrentVersion();
  auto head         = ensureSetting(*layout, before,
                                    sliceSubviewSettingSpec(id, descriptor_.kind));
  head =
      setSetting(*layout, head, settings::sliceSubviewKey(id, descriptor_.kind),
                 std::string(subviewId()));
  if (head != before && host_.repoint) {
    host_.repoint(SystemDocKind::Layout, head);
  }
}

void ViewSlicePresentation::persistBindings() {
  if (!space_ || nullptr == store_ || !host_.systemStore) {
    return;
  }
  auto *const layout = host_.systemStore(SystemDocKind::Layout);
  if (nullptr == layout) {
    return;
  }
  const auto id     = store_->documentId().str();
  const auto cells  = cellsOf(saveBindings(*space_, *store_));
  const auto model  = SystemStoreModel::fromStore(*layout);
  const auto before = layout->primaryCurrentVersion();
  auto head         = before;
  const auto specs  = sliceBindingSettingSpecs(id);
  const auto write  = [&](const SettingSpec &spec,
                         const std::vector<std::string> &strings) {
    if (model.find(spec.name) && model.getStringList(spec.name) == strings) {
      return;
    }
    head = ensureSetting(*layout, head, spec);
    std::vector<CellValue> values(strings.begin(), strings.end());
    head = setSetting(*layout, head, spec.name,
                       std::span<const CellValue>{values});
  };
  write(specs[0], cells.axes);
  write(specs[1], cells.groups);
  write(specs[2], cells.ringOrder);
  if (head != before && host_.repoint) {
    host_.repoint(SystemDocKind::Layout, head);
  }
}

void ViewSlicePresentation::persistFocus() {
  focusDirty_ = false;
  if (nullptr == store_ || !host_.systemStore ||
      zigzag::noCell == cursor_.origin) {
    return;
  }
  auto *const layout = host_.systemStore(SystemDocKind::Layout);
  if (nullptr == layout) {
    return;
  }
  const auto id    = store_->documentId().str();
  const auto cell  = static_cast<std::int64_t>(cursor_.origin);
  const auto model = SystemStoreModel::fromStore(*layout);
  const auto kept  = model.getInt64List(settings::sliceFocusKey(id));
  if (!kept.empty() && kept.front() == cell) {
    return;
  }
  const auto before = layout->primaryCurrentVersion();
  const auto spec   = sliceFocusSettingSpec(id);
  auto head         = ensureSetting(*layout, before, spec);
  head              = setSetting(*layout, head, spec.name, cell);
  if (head != before && host_.repoint) {
    host_.repoint(SystemDocKind::Layout, head);
  }
}

void ViewSlicePresentation::refreshStatus() {
  auto words = std::string(descriptor_.name);
  if (subview_ < descriptor_.subviews.size()) {
    words += ": " + descriptor_.subviews[subview_].name;
  }
  if (words != status_) {
    status_ = std::move(words);
    ++chromeRevision_;
    ++revision_;
  }
}

std::span<const CompassArm> ViewSlicePresentation::compass() const {
  ensureSpace();
  return compass_;
}

ViewAxisSet *ViewSlicePresentation::axisSet() const {
  ensureSpace();
  return space_ ? &space_->axes() : nullptr;
}

void ViewSlicePresentation::arrive(const MoveOutcome &outcome,
                                   const bool recorded) {
  cursor_ = outcome.cursor;
  if (outcome.originChanged) {
    // Only the derived arena goes: the bindings are the reader's, and a point
    // that shows nothing here stays so until the reader binds it (§7.3).
    space_->toss();
  }
  focusDirty_ = true;
  if (recorded) {
    if (const auto cell = cellAt(*space_, cursor_)) {
      record(*cell);
    }
  }
  if (!notice_.empty()) {
    notice_.clear();
    ++chromeRevision_;
  }
  if (outcome.originChanged) {
    bindWhatIsLeftUnbound();
  }
  cause_ = MotionCause::Move;
  stale_ = true;
  changed();
}

void ViewSlicePresentation::bindWhatIsLeftUnbound() {
  // The one case where arriving binds anything: no spatial point shows
  // anything, as in a slice that opened on a cell linking along nothing. With
  // a single point bound there is a choice of the reader's to keep, so
  // nothing is filled in beside it. What is bound is said.
  auto &axes = space_->axes();
  for (ViewAxisId axis = 0; axis < axes.axisCount(); ++axis) {
    const auto role = axes.role(axis);
    if (role && role->spatial() && axes.shown(axis)) {
      return;
    }
  }
  seedAxes({});
  syncLegacyView();
  mintCues();
  refreshCompass();
  std::string words;
  for (ViewAxisId axis = 0; axis < axes.axisCount(); ++axis) {
    if (const auto name = shownName(axis); !name.empty()) {
      const auto point = axes.pointName(axis).value_or("?");
      const std::array fields{MessageField{"target", name},
                              MessageField{"axis", point}};
      words += (words.empty() ? "" : " ") +
               spokenMessage(ViewMessage::AxisShows, fields);
    }
  }
  if (!words.empty()) {
    notice_ = std::move(words);
    ++chromeRevision_;
    ++revision_;
  }
}

void ViewSlicePresentation::goTo(const zigzag::CellRef cell,
                                 const bool recorded) {
  ensureSpace();
  if (!space_ || !manifold_->contains(cell)) {
    wantedFocus_ = cell;
    return;
  }
  if (cursor_ == SliceCursor{.origin = cell}) {
    return;
  }
  arrive(MoveOutcome{.cursor        = SliceCursor{.origin = cell},
                     .moved         = true,
                     .originChanged = true},
         recorded);
}

void ViewSlicePresentation::record(const zigzag::CellRef cell) {
  if (nullptr == store_) {
    return;
  }
  // A completed move is a visit (view-system.md §11.6), with the real cell
  // as its target: never a view cell.
  const auto id = log_->append(Visit{
      .parent  = log_->current(),
      .target  = CellSite{.store   = store_->documentId(),
                          .version = head_,
                          .cell    = cell,
                          .range   = {.start = 0,
                                      .end   = static_cast<std::uint32_t>(
                                       textOf(SubjectId::cell(cell)).size())}},
      .arrival = Arrival::Opened});
  log_->select(id);
  refreshBreadcrumbs();
}

void ViewSlicePresentation::refreshBreadcrumbs() {
  std::vector<std::string> trail;
  auto at = log_->current();
  while (at && trail.size() < stretch_.breadcrumbs) {
    const auto visit = log_->find(*at);
    if (!visit) {
      break;
    }
    if (const auto *const site = std::get_if<CellSite>(&visit->target);
        nullptr != site && nullptr != store_ &&
        site->store == store_->documentId() && manifold_ &&
        manifold_->contains(site->cell)) {
      trail.emplace_back(textOf(SubjectId::cell(site->cell)));
    }
    at = visit->parent;
  }
  std::ranges::reverse(trail);
  if (trail != breadcrumbs_) {
    breadcrumbs_ = std::move(trail);
    ++chromeRevision_;
  }
}

void ViewSlicePresentation::say(const ViewMessage message,
                                const std::span<const MessageField> fields) {
  notice_ = spokenMessage(message, fields);
  ++chromeRevision_;
  ++revision_;
}

void ViewSlicePresentation::changed() {
  ++revision_;
  if (invalidation_) {
    invalidation_(revision_);
  }
}

std::string ViewSlicePresentation::notice() const { return notice_; }

std::vector<std::string> ViewSlicePresentation::breadcrumbs() const {
  return breadcrumbs_;
}

// -- picking and accessibility ------------------------------------------------

bool ViewSlicePresentation::picked(const render::PickingResult &pick,
                                   RenderState & /*state*/) {
  if (!visible_) {
    return false;
  }
  const auto subject = presenter_.subjectOf(pick);
  if (!subject || !(SubjectKind::Cell == subject->kind ||
                    SubjectKind::Marker == subject->kind)) {
    return false;
  }
  // A click focuses the cell clicked, or the one a ghost stands for (§9.1.7).
  goTo(static_cast<zigzag::CellRef>(subject->value), true);
  return true;
}

std::uint64_t ViewSlicePresentation::accessibilityRevision() const {
  return revision_ + chromeRevision_ + presenter_.projectionRevision() +
         keyboardMoves_.load();
}

void ViewSlicePresentation::describe(gleditor::a11y::Builder &into) {
  using gleditor::a11y::Action;
  using gleditor::a11y::Role;
  if (!visible_) {
    return;
  }
  const std::scoped_lock lock(a11yLock_);
  a11yTargets_.clear();
  std::vector<std::uint64_t> children;

  const auto items = presenter_.drawnItems();
  const auto edges = drawnSink_.edges();
  const auto idOf  = [this](const SubjectId subject) {
    auto [found, made] =
        a11yIds_.try_emplace(subject, kFirstNode + nextA11yId_);
    if (made) {
      ++nextA11yId_;
    }
    a11yTargets_[found->second] = subject;
    return found->second;
  };
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto &item = items[i];
    const bool ghost = presenter_.drawnAsGhost(i);
    const auto words = std::string(textOf(item.id));
    if (SubjectKind::Cell == item.id.kind && !ghost) {
      const auto local = idOf(item.id);
      auto &node       = into.add(local, Role::ListItem);
      // The name is the full text, whatever was painted (V-R43).
      node.label            = words;
      std::string relations = "cell";
      for (const auto &edge : edges) {
        const bool from = edge.from == item.id;
        if (!from && edge.to != item.id) {
          continue;
        }
        const auto &other = from ? edge.to : edge.from;
        relations += std::format(
            "; {} {}: {}", textOf(SubjectId::label(edge.relation)),
            from ? "posward" : "negward",
            textOf(SubjectId::cell(static_cast<zigzag::CellRef>(other.value))));
      }
      node.description = std::move(relations);
      node.actions     = gleditor::a11y::bit(Action::Click) |
                     gleditor::a11y::bit(Action::Focus);
      node.focusable = true;
      node.readOnly  = true;
      node.bounds    = presenter_.screenBounds(i);
      children.push_back(into.id(local));
      if (0U != (item.flags & itemFocus) && keyboardHere_.load()) {
        into.takeFocus(into.id(local));
      }
    } else if (ghost) {
      // View-only: never announced as stored content (V-R45).
      const auto local = idOf(SubjectKind::Marker == item.id.kind
                                  ? item.id
                                  : markId(MarkKind::Ghost, item.id));
      a11yTargets_[local] =
          SubjectId::cell(static_cast<zigzag::CellRef>(item.id.value));
      auto &node   = into.add(local, Role::Group);
      node.label   = "A cell beyond the edge of the view";
      node.actions = gleditor::a11y::bit(Action::Click);
      node.bounds  = presenter_.screenBounds(i);
      children.push_back(into.id(local));
    } else if (SubjectKind::Label == item.id.kind) {
      const auto local = idOf(item.id);
      auto &node       = into.add(local, Role::Label);
      node.label       = words;
      node.bounds      = presenter_.screenBounds(i);
      children.push_back(into.id(local));
    }
  }

  if (!breadcrumbs_.empty()) {
    std::vector<std::uint64_t> crumbs;
    for (std::size_t i = 0; i < breadcrumbs_.size(); ++i) {
      const auto local = kFirstCrumbNode + i;
      auto &crumb      = into.add(local, Role::ListItem);
      crumb.label      = breadcrumbs_[i];
      crumbs.push_back(into.id(local));
    }
    auto &trail    = into.add(kTrailNode, Role::List);
    trail.label    = "Cells walked";
    trail.children = std::move(crumbs);
    children.push_back(into.id(kTrailNode));
  }
  if (compassConfig_.visible && !compass_.empty()) {
    std::vector<std::uint64_t> arms;
    for (std::size_t i = 0; i < compass_.size(); ++i) {
      const auto local = kFirstArmNode + i;
      auto &arm        = into.add(local, Role::ListItem);
      arm.label        = compass_[i].words();
      arms.push_back(into.id(local));
    }
    auto &rose    = into.add(kCompassNode, Role::List);
    rose.label    = "Binding points";
    rose.children = std::move(arms);
    children.push_back(into.id(kCompassNode));
  }
  if (!status_.empty()) {
    auto &status = into.add(kStatusNode, Role::Label);
    status.label = status_;
    children.push_back(into.id(kStatusNode));
  }
  if (!notice_.empty()) {
    auto &notice = into.add(kNoticeNode, Role::Label);
    notice.label = notice_;
    notice.live  = gleditor::a11y::Live::Polite;
    children.push_back(into.id(kNoticeNode));
  }

  auto &root = into.add(kRootNode, Role::Group);
  root.label = std::format("ZigZag slice, {}", descriptor_.name);
  std::string axes;
  if (space_) {
    for (ViewAxisId axis = 0; axis < space_->axes().axisCount(); ++axis) {
      if (auto name = shownName(axis); !name.empty()) {
        axes += std::format("{}{} shows {}", axes.empty() ? "" : ", ",
                            space_->axes().pointName(axis).value_or("?"), name);
      }
    }
  }
  root.description = std::move(axes);
  root.children    = std::move(children);
  into.contribute(into.id(kRootNode));
}

bool ViewSlicePresentation::performAction(const std::uint64_t nodeId,
                                          const gleditor::a11y::Action action,
                                          const std::string_view /*value*/) {
  if (!visible_ || (gleditor::a11y::Action::Click != action &&
                    gleditor::a11y::Action::Focus != action)) {
    return false;
  }
  std::optional<SubjectId> subject;
  {
    const std::scoped_lock lock(a11yLock_);
    const auto found = a11yTargets_.find(gleditor::a11y::Ids::localOf(nodeId));
    if (found != a11yTargets_.end() &&
        SubjectKind::Cell == found->second.kind) {
      subject = found->second;
    }
  }
  if (!subject) {
    return false;
  }
  runOnOwner([this, cell = static_cast<zigzag::CellRef>(subject->value)] {
    goTo(cell, true);
  });
  return true;
}

// -- the rest of the seam
// -------------------------------------------------------

ViewSlicePresentation *
ViewSlicePresentation::setPresentationVisible(const bool visible) {
  if (visible_ != visible) {
    visible_ = visible;
    stale_   = true;
    changed();
  }
  return this;
}

ViewSlicePresentation *
ViewSlicePresentation::setPresentationOrigin(const glm::vec3 origin) {
  origin_ = origin;
  if (!resolver_) {
    toWorld_ = originTransform(origin_);
  }
  changed();
  return this;
}

ViewSlicePresentation *ViewSlicePresentation::setPresentationTransformResolver(
    PresentationTransformResolver resolver) {
  resolver_ = std::move(resolver);
  if (!resolver_) {
    readableScale_ = 1.0F;
  }
  return this;
}

ViewSlicePresentation *
ViewSlicePresentation::setPresentationConfig(ZigzagPresentationConfig config) {
  if (!(config == cells_)) {
    cells_ = config;
    applySettings();
  }
  return this;
}

ViewSlicePresentation *ViewSlicePresentation::setPreviewCell(
    const std::optional<zigzag::CellRef> cell) {
  preview_ = cell;
  return this;
}

ViewSlicePresentation *
ViewSlicePresentation::setExternInspector(ExternInspector inspector) {
  externInspector_ = std::move(inspector);
  return this;
}

ViewSlicePresentation *
ViewSlicePresentation::setHasKeyboard(const bool has) noexcept {
  if (keyboardHere_.exchange(has) != has) {
    ++keyboardMoves_;
  }
  return this;
}

ViewSlicePresentation *
ViewSlicePresentation::setKeyHints(std::string here, std::string elsewhere) {
  hintsHere_      = std::move(here);
  hintsElsewhere_ = std::move(elsewhere);
  ++chromeRevision_;
  return this;
}

bool ViewSlicePresentation::insertConnectedTransclusion(
    const std::span<const PrimediaSpan> /*spans*/) {
  runOnOwner([this] { refused("insert connected transclusion"); });
  return false;
}

zigzag::CellRef ViewSlicePresentation::focusCell() const noexcept {
  if (wantedFocus_) {
    return *wantedFocus_;
  }
  return cursor_.origin;
}

void ViewSlicePresentation::focusCell(const zigzag::CellRef cell) {
  // The host moving the focus (a link followed, a satelloid chosen) is its
  // own visit, recorded by the host's activity; not a second one here.
  goTo(cell, false);
}

void ViewSlicePresentation::activateCell(const zigzag::CellRef cell) {
  if (activation_) {
    activation_(cell);
  }
}

void ViewSlicePresentation::setCellRadius(const int radius) noexcept {
  cellRadius_ = std::max(1, radius);
  changed();
}

bool ViewSlicePresentation::isCellLocked(
    const zigzag::CellRef /*cell*/) const noexcept {
  return false;
}

std::optional<TranscopyrightDescriptor> ViewSlicePresentation::cellRoyalty(
    const zigzag::CellRef /*cell*/) const noexcept {
  return std::nullopt;
}

bool ViewSlicePresentation::unlockCell(const zigzag::CellRef /*cell*/) {
  return false;
}

std::optional<CellAnchor>
ViewSlicePresentation::cellAnchor(const zigzag::CellRef cell) const {
  const auto items = sink_.items();
  const auto found =
      std::ranges::find(items, SubjectId::cell(cell), &PlacedItem::id);
  if (found == items.end() || 0U != (found->flags & itemGhost)) {
    return std::nullopt;
  }
  const float xScale = glm::length(glm::vec3(toWorld_[0]));
  const float yScale = glm::length(glm::vec3(toWorld_[1]));
  const float line   = presenter_.bodyLineHeight();
  return CellAnchor{.position =
                        glm::vec3(toWorld_ * glm::vec4(found->centre, 1.0F)),
                    .width      = found->width * xScale,
                    .height     = found->height * yScale,
                    .lineHeight = line * yScale,
                    .normal     = glm::vec3(0.0F, 0.0F, 1.0F)};
}

std::optional<glm::vec3> ViewSlicePresentation::focusCentre() const {
  // The focus is always laid out at the placement's origin (§9.1.3).
  return glm::vec3(toWorld_ * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F));
}

void ViewSlicePresentation::setCellHighlights(
    std::vector<CellHighlight> highlights, const std::uint32_t borderColour) {
  highlights_      = std::move(highlights);
  highlightBorder_ = borderColour;
  ++stamp_;
}

} // namespace xanadu::view

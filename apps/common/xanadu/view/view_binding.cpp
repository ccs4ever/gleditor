/**
 * @file view_binding.cpp
 * @brief The binding model over a placement's binding arena
 *        (design/view-system.md §7, §8.3).
 */
#include "common/xanadu/view/view_binding.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

#include <gleditor/logging.hpp>

#include "common/xanadu/spool.hpp"

namespace xanadu::view {

namespace {

constexpr auto pos = zigzag::DimVector::POS;
constexpr auto neg = zigzag::DimVector::NEG;

class SpatialRole final : public AxisRole {
public:
  [[nodiscard]] std::string_view id() const noexcept override {
    return "spatial";
  }
  [[nodiscard]] bool spatial() const noexcept override { return true; }
};

class SubspaceRole final : public AxisRole {
public:
  [[nodiscard]] std::string_view id() const noexcept override {
    return "subspace";
  }
  [[nodiscard]] bool spatial() const noexcept override { return false; }
};

class HypertimeRole final : public AxisRole {
public:
  [[nodiscard]] std::string_view id() const noexcept override {
    return "hypertime";
  }
  [[nodiscard]] bool spatial() const noexcept override { return false; }
};

} // namespace

const AxisRole &spatialRole() noexcept {
  static const SpatialRole role;
  return role;
}

const AxisRole &subspaceRole() noexcept {
  static const SubspaceRole role;
  return role;
}

const AxisRole &hypertimeRole() noexcept {
  static const HypertimeRole role;
  return role;
}

gleditor::cpp26::optional<const AxisRole &>
roleNamed(const std::string_view id) noexcept {
  for (const auto *const role :
       {&spatialRole(), &subspaceRole(), &hypertimeRole()}) {
    if (role->id() == id) {
      return *role;
    }
  }
  return {};
}

std::vector<BindingPoint>
bindingPointsFrom(const std::span<const CellValue> setting) {
  std::vector<BindingPoint> points;
  for (std::size_t i = 0; i < setting.size(); i += 2) {
    const auto *const name = std::get_if<std::string>(&setting[i]);
    if (i + 1 >= setting.size()) {
      GLEDITOR_LOG_WARN("view.binding", "binding point {} has no role; skipped",
                        i / 2);
      break;
    }
    const auto *const roleId = std::get_if<std::string>(&setting[i + 1]);
    const auto role          = roleId != nullptr
                                   ? roleNamed(*roleId)
                                   : gleditor::cpp26::optional<const AxisRole &>{};
    if (name == nullptr || name->empty() || !role.has_value()) {
      GLEDITOR_LOG_WARN("view.binding",
                        "binding point {} is not a name and a known role; "
                        "skipped",
                        i / 2);
      continue;
    }
    points.push_back(BindingPoint{.name = *name, .role = std::cref(*role)});
  }
  return points;
}

// -- the edit in progress -----------------------------------------------------

/// The steps one edit has applied so far, so a refusal part way can put
/// every one back and a success can be pushed for undo() whole.
class ViewAxisSet::Edit {
public:
  explicit Edit(ViewAxisSet &set, const bool record = true) noexcept
      : set_(set), record_(record) {}

  std::expected<void, ViewError> link(const zigzag::CellRef from,
                                      const zigzag::CellRef dim,
                                      const zigzag::DimVector dir,
                                      const zigzag::CellRef to) {
    return run(LinkStep{.from = from, .dim = dim, .dir = dir, .to = to});
  }

  std::expected<void, ViewError> unlink(const zigzag::CellRef from,
                                        const zigzag::CellRef dim,
                                        const zigzag::DimVector dir) {
    const auto there = set_.next(from, dim, dir);
    if (!there.has_value()) {
      return {};
    }
    return run(UnlinkStep{.from = from, .dim = dim, .dir = dir, .to = *there});
  }

  std::expected<void, ViewError> text(const zigzag::CellRef cell,
                                      const std::string_view after) {
    return run(TextStep{.cell   = cell,
                        .before = set_.textOf(cell).value_or(std::string{}),
                        .after  = std::string{after}});
  }

  /// A ring move already made by relinkRing().
  void ring(const RingStep step) {
    if (record_) {
      steps_.emplace_back(step);
    }
  }

  std::expected<void, ViewError> insertAfter(const zigzag::CellRef anchor,
                                             const zigzag::CellRef dim,
                                             const zigzag::CellRef cell) {
    const auto after = set_.next(anchor, dim);
    if (after.has_value()) {
      if (auto done = unlink(anchor, dim, pos); !done) {
        return done;
      }
    }
    if (auto done = link(anchor, dim, pos, cell); !done) {
      return done;
    }
    return after.has_value() ? link(cell, dim, pos, *after)
                             : std::expected<void, ViewError>{};
  }

  std::expected<void, ViewError> detach(const zigzag::CellRef cell,
                                        const zigzag::CellRef dim) {
    const auto before = set_.next(cell, dim, neg);
    const auto after  = set_.next(cell, dim, pos);
    if (before.has_value()) {
      if (auto done = unlink(*before, dim, pos); !done) {
        return done;
      }
    }
    if (after.has_value()) {
      if (auto done = unlink(cell, dim, pos); !done) {
        return done;
      }
    }
    return before.has_value() && after.has_value()
               ? link(*before, dim, pos, *after)
               : std::expected<void, ViewError>{};
  }

  void rollback() {
    for (auto step = steps_.rbegin(); step != steps_.rend(); ++step) {
      std::ignore = set_.apply(*step, false);
    }
    steps_.clear();
  }

  [[nodiscard]] bool empty() const noexcept { return steps_.empty(); }
  [[nodiscard]] Entry take() noexcept { return std::move(steps_); }

private:
  std::expected<void, ViewError> run(Step step) {
    auto done = set_.apply(step, true);
    if (done && record_) {
      steps_.push_back(std::move(step));
    }
    return done;
  }

  ViewAxisSet &set_;
  bool record_;
  Entry steps_;
};

// -- reads --------------------------------------------------------------------

ViewCellRef ViewAxisSet::cell(const zigzag::CellRef ref) noexcept {
  return ViewCellRef{
      .ref = ref, .epoch = bindingEpoch, .layer = Layer::Binding};
}

std::optional<zigzag::CellRef>
ViewAxisSet::next(const zigzag::CellRef from, const zigzag::CellRef dim,
                  const zigzag::DimVector dir) const noexcept {
  const auto found = space_->linked(cell(from), cell(dim), dir);
  if (!found.has_value()) {
    return std::nullopt;
  }
  return found->ref;
}

std::optional<zigzag::CellRef>
ViewAxisSet::targetOf(const zigzag::CellRef occurrence) const noexcept {
  return space_->target(cell(occurrence));
}

std::optional<std::string>
ViewAxisSet::textOf(const zigzag::CellRef ref) const {
  return space_->text(cell(ref));
}

void ViewAxisSet::walk(
    const zigzag::CellRef head, const zigzag::CellRef dim,
    gleditor::cpp26::function_ref<bool(zigzag::CellRef)> visit) const {
  auto budget = space_->cellCount(Layer::Binding);
  for (auto cur = next(head, dim); cur.has_value() && budget > 0;
       cur      = next(*cur, dim), --budget) {
    if (!visit(*cur)) {
      return;
    }
  }
}

std::optional<zigzag::CellRef>
ViewAxisSet::rankAt(const zigzag::CellRef head, const zigzag::CellRef dim,
                    const std::size_t position) const {
  std::optional<zigzag::CellRef> found;
  std::size_t at = 0;
  walk(head, dim, [&](const zigzag::CellRef cur) {
    if (at++ == position) {
      found = cur;
      return false;
    }
    return true;
  });
  return found;
}

std::size_t ViewAxisSet::rankSize(const zigzag::CellRef head,
                                  const zigzag::CellRef dim) const {
  std::size_t size = 0;
  walk(head, dim, [&](zigzag::CellRef) {
    ++size;
    return true;
  });
  return size;
}

zigzag::CellRef ViewAxisSet::tailOf(const zigzag::CellRef head,
                                    const zigzag::CellRef dim) const {
  auto tail = head;
  walk(head, dim, [&](const zigzag::CellRef cur) {
    tail = cur;
    return true;
  });
  return tail;
}

std::optional<ViewAxisSet::Found>
ViewAxisSet::findOccurrence(const zigzag::CellRef head,
                            const zigzag::CellRef dim,
                            const zigzag::CellRef target) const {
  std::optional<Found> found;
  std::size_t place = 0;
  walk(head, dim, [&](const zigzag::CellRef cur) {
    if (targetOf(cur) == target) {
      found = Found{.cell = cur, .place = place};
      return false;
    }
    ++place;
    return true;
  });
  return found;
}

std::optional<zigzag::CellRef>
ViewAxisSet::slotAt(const ViewAxisId axis) const {
  if (!frame_.has_value()) {
    return std::nullopt;
  }
  return rankAt(frame_->axesHead, frame_->axes, axis);
}

std::optional<zigzag::DimRef>
ViewAxisSet::realDimension(const zigzag::CellRef target) const {
  if (zigzag::isEphemeral(target)) {
    return std::nullopt;
  }
  const auto slot = space_->base().slot(target);
  if (!slot) {
    return std::nullopt;
  }
  // The cell's own name, so that a later micro-history op of it is the same
  // dimension and every occurrence of it holds one value.
  const zigzag::DimRef own = slot->birthOp;
  if (!std::ranges::contains(space_->base().dimensions(), own)) {
    return std::nullopt;
  }
  return own;
}

std::optional<BindTarget> ViewAxisSet::bindable(const BindTarget target) const {
  if (const auto dim = realDimension(target); dim.has_value()) {
    return dim;
  }
  if (isGroup(target)) {
    return target;
  }
  return std::nullopt;
}

bool ViewAxisSet::isGroup(const BindTarget target) const noexcept {
  // A live group is on the group list, after its head; a deleted one is not.
  return frame_.has_value() && zigzag::isEphemeral(target) &&
         next(target, frame_->groups, neg).has_value();
}

std::size_t ViewAxisSet::groupCount() const {
  return frame_.has_value() ? rankSize(frame_->groupsHead, frame_->groups) : 0;
}

bool ViewAxisSet::reaches(const BindTarget outer,
                          const BindTarget inner) const {
  if (!frame_.has_value()) {
    return false;
  }
  std::vector<zigzag::CellRef> pending{outer};
  std::vector<zigzag::CellRef> seen;
  bool found = false;
  while (!pending.empty() && !found) {
    const auto group = pending.back();
    pending.pop_back();
    if (std::ranges::contains(seen, group)) {
      continue;
    }
    seen.push_back(group);
    walk(group, frame_->members, [&](const zigzag::CellRef occurrence) {
      const auto member = targetOf(occurrence);
      if (member.has_value() && isGroup(*member)) {
        found = *member == inner;
        pending.push_back(*member);
      }
      return !found;
    });
  }
  return found;
}

void ViewAxisSet::forEachLeaf(
    const BindTarget target,
    gleditor::cpp26::function_ref<void(zigzag::DimRef)> visit) const {
  if (const auto dim = realDimension(target); dim.has_value()) {
    visit(*dim);
    return;
  }
  if (!isGroup(target)) {
    return;
  }
  // Depth first without recursion, one cursor per open group. Cycles are
  // refused, but a nesting deeper than there are groups can only be a loop
  // made behind the set's back, so the depth is bounded by the group count.
  const auto deepest = groupCount() + 1;
  std::vector<std::optional<zigzag::CellRef>> cursors{
      next(target, frame_->members)};
  while (!cursors.empty()) {
    const auto occurrence = cursors.back();
    if (!occurrence.has_value()) {
      cursors.pop_back();
      continue;
    }
    cursors.back()    = next(*occurrence, frame_->members);
    const auto member = targetOf(*occurrence);
    if (!member.has_value()) {
      continue;
    }
    if (isGroup(*member)) {
      if (cursors.size() < deepest) {
        cursors.push_back(next(*member, frame_->members));
      }
    } else {
      visit(*member);
    }
  }
}

bool ViewAxisSet::hasLeaf(const BindTarget target) const {
  bool any = false;
  forEachLeaf(target, [&](zigzag::DimRef) { any = true; });
  return any;
}

// -- the frame ---------------------------------------------------------------

std::expected<const ViewAxisSet::Frame *, ViewError>
ViewAxisSet::ensureFrame() {
  if (frame_.has_value()) {
    return &*frame_;
  }
  Frame frame{};
  const std::array<std::pair<zigzag::CellRef *, std::string_view>, 7> dims{{
      {&frame.axes, "d.axes"},
      {&frame.roles, "d.axis-role"},
      {&frame.binds, "d.binds"},
      {&frame.groups, "d.view-groups"},
      {&frame.members, "d.dim-group"},
      {&frame.ring, "d.ring-order"},
      {&frame.pouch, "d.dim-pouch"},
  }};
  for (const auto &[slot, name] : dims) {
    const auto minted = space_->mint(Layer::Binding, name);
    if (!minted) {
      return std::unexpected{minted.error()};
    }
    *slot = minted->ref;
  }
  for (auto *const head : {&frame.axesHead, &frame.groupsHead, &frame.ringHead,
                           &frame.pouchHead}) {
    const auto minted = space_->mint(Layer::Binding);
    if (!minted) {
      return std::unexpected{minted.error()};
    }
    *head = minted->ref;
  }
  frame_ = frame;
  // G11: the ring order begins as the slice's own dimension order, so where
  // the reader happened to go first does not decide it.
  auto tail = frame.ringHead;
  for (const auto dim : space_->base().dimensions()) {
    const auto occurrence = space_->mintOccurrence(Layer::Binding, dim);
    if (!occurrence) {
      continue;
    }
    if (!space_->link(Layer::Binding, cell(tail), cell(frame.ring), pos,
                      *occurrence)) {
      return std::unexpected{ViewError::ArenaRefused};
    }
    tail = occurrence->ref;
  }
  return &*frame_;
}

// -- edits, undo and redo ----------------------------------------------------

std::expected<void, ViewError> ViewAxisSet::apply(const Step &step,
                                                  const bool forward) {
  const auto done =
      [](const ViewResult &result) -> std::expected<void, ViewError> {
    if (!result) {
      return std::unexpected{result.error()};
    }
    return {};
  };
  if (const auto *const link = std::get_if<LinkStep>(&step)) {
    return forward
               ? done(space_->link(Layer::Binding, cell(link->from),
                                   cell(link->dim), link->dir, cell(link->to)))
               : done(space_->unlink(Layer::Binding, cell(link->from),
                                     cell(link->dim), link->dir));
  }
  if (const auto *const unlink = std::get_if<UnlinkStep>(&step)) {
    return forward ? done(space_->unlink(Layer::Binding, cell(unlink->from),
                                         cell(unlink->dim), unlink->dir))
                   : done(space_->link(Layer::Binding, cell(unlink->from),
                                       cell(unlink->dim), unlink->dir,
                                       cell(unlink->to)));
  }
  if (const auto *const text = std::get_if<TextStep>(&step)) {
    return done(space_->setText(Layer::Binding, cell(text->cell),
                                forward ? text->after : text->before));
  }
  const auto &ring = std::get<RingStep>(step);
  return relinkRing(ring.dimension, forward ? ring.to : ring.from);
}

AxisResult ViewAxisSet::finish(Edit &edit,
                               const std::expected<void, ViewError> outcome) {
  if (!outcome) {
    edit.rollback();
    return std::unexpected{outcome.error()};
  }
  if (!edit.empty()) {
    undo_.push_back(edit.take());
    redo_.clear();
  }
  return this;
}

bool ViewAxisSet::undo() {
  if (undo_.empty()) {
    return false;
  }
  auto entry = std::move(undo_.back());
  undo_.pop_back();
  for (std::size_t i = entry.size(); i-- > 0;) {
    if (!apply(entry[i], false)) {
      for (std::size_t j = i + 1; j < entry.size(); ++j) {
        std::ignore = apply(entry[j], true);
      }
      undo_.push_back(std::move(entry));
      return false;
    }
  }
  redo_.push_back(std::move(entry));
  return true;
}

bool ViewAxisSet::redo() {
  if (redo_.empty()) {
    return false;
  }
  auto entry = std::move(redo_.back());
  redo_.pop_back();
  for (std::size_t i = 0; i < entry.size(); ++i) {
    if (!apply(entry[i], true)) {
      for (std::size_t j = i; j-- > 0;) {
        std::ignore = apply(entry[j], false);
      }
      redo_.push_back(std::move(entry));
      return false;
    }
  }
  undo_.push_back(std::move(entry));
  return true;
}

ViewAxisSet *ViewAxisSet::forgetHistory() noexcept {
  undo_.clear();
  redo_.clear();
  return this;
}

// -- axes --------------------------------------------------------------------

std::size_t ViewAxisSet::axisCount() const noexcept {
  return frame_.has_value() ? rankSize(frame_->axesHead, frame_->axes) : 0;
}

std::expected<ViewAxisId, ViewError>
ViewAxisSet::addAxis(const std::string_view name, const AxisRole &role) {
  const auto frame = ensureFrame();
  if (!frame) {
    return std::unexpected{frame.error()};
  }
  const auto &f = **frame;
  const auto id = static_cast<ViewAxisId>(axisCount());
  Edit edit{*this};
  const auto outcome = [&]() -> std::expected<void, ViewError> {
    const auto slot     = space_->mint(Layer::Binding, name);
    const auto roleCell = space_->mint(Layer::Binding, role.id());
    if (!slot || !roleCell) {
      return std::unexpected{ViewError::ArenaRefused};
    }
    if (auto done = edit.link(slot->ref, f.roles, pos, roleCell->ref); !done) {
      return done;
    }
    return edit.link(tailOf(f.axesHead, f.axes), f.axes, pos, slot->ref);
  }();
  if (const auto done = finish(edit, outcome); !done) {
    return std::unexpected{done.error()};
  }
  return id;
}

AxisResult ViewAxisSet::configure(const std::span<const BindingPoint> points) {
  for (const auto &point : points) {
    if (axisNamed(point.name).has_value()) {
      continue;
    }
    if (const auto added = addAxis(point.name, point.role.get()); !added) {
      return std::unexpected{added.error()};
    }
  }
  return forgetHistory();
}

AxisResult ViewAxisSet::removeAxis(const ViewAxisId axis) {
  const auto slot = slotAt(axis);
  if (!slot.has_value()) {
    return std::unexpected{ViewError::UnknownAxis};
  }
  const auto &f = *frame_;
  Edit edit{*this};
  const auto outcome = [&]() -> std::expected<void, ViewError> {
    if (auto done = edit.unlink(*slot, f.binds, pos); !done) {
      return done;
    }
    if (auto done = edit.unlink(*slot, f.roles, pos); !done) {
      return done;
    }
    return edit.detach(*slot, f.axes);
  }();
  return finish(edit, outcome);
}

AxisResult ViewAxisSet::bind(const ViewAxisId axis, const BindTarget target) {
  const auto slot = slotAt(axis);
  if (!slot.has_value()) {
    return std::unexpected{ViewError::UnknownAxis};
  }
  const auto shows = bindable(target);
  if (!shows.has_value()) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  if (isGroup(*shows) && !hasLeaf(*shows)) {
    return std::unexpected{ViewError::EmptyGroupBind};
  }
  if (shown(axis) == shows) {
    return this;
  }
  const auto &f = *frame_;
  Edit edit{*this};
  const auto outcome = [&]() -> std::expected<void, ViewError> {
    if (auto done = edit.unlink(*slot, f.binds, pos); !done) {
      return done;
    }
    const auto occurrence = space_->mintOccurrence(Layer::Binding, *shows);
    if (!occurrence) {
      return std::unexpected{occurrence.error()};
    }
    return edit.link(*slot, f.binds, pos, occurrence->ref);
  }();
  return finish(edit, outcome);
}

AxisResult ViewAxisSet::unbind(const ViewAxisId axis) {
  const auto slot = slotAt(axis);
  if (!slot.has_value()) {
    return std::unexpected{ViewError::UnknownAxis};
  }
  Edit edit{*this};
  return finish(edit, edit.unlink(*slot, frame_->binds, pos));
}

AxisResult ViewAxisSet::swap(const ViewAxisId first, const ViewAxisId second) {
  const auto a = slotAt(first);
  const auto b = slotAt(second);
  if (!a.has_value() || !b.has_value()) {
    return std::unexpected{ViewError::UnknownAxis};
  }
  if (first == second) {
    return this;
  }
  const auto binds = frame_->binds;
  const auto onA   = next(*a, binds);
  const auto onB   = next(*b, binds);
  Edit edit{*this};
  const auto outcome = [&]() -> std::expected<void, ViewError> {
    if (auto done = edit.unlink(*a, binds, pos); !done) {
      return done;
    }
    if (auto done = edit.unlink(*b, binds, pos); !done) {
      return done;
    }
    if (onB.has_value()) {
      if (auto done = edit.link(*a, binds, pos, *onB); !done) {
        return done;
      }
    }
    return onA.has_value() ? edit.link(*b, binds, pos, *onA)
                           : std::expected<void, ViewError>{};
  }();
  return finish(edit, outcome);
}

std::optional<BindTarget>
ViewAxisSet::shown(const ViewAxisId axis) const noexcept {
  const auto slot = slotAt(axis);
  if (!slot.has_value()) {
    return std::nullopt;
  }
  const auto occurrence = next(*slot, frame_->binds);
  return occurrence.has_value() ? targetOf(*occurrence) : std::nullopt;
}

std::optional<ViewAxisId>
ViewAxisSet::axisNamed(const std::string_view name) const {
  if (!frame_.has_value()) {
    return std::nullopt;
  }
  std::optional<ViewAxisId> found;
  ViewAxisId at = 0;
  walk(frame_->axesHead, frame_->axes, [&](const zigzag::CellRef slot) {
    if (textOf(slot) == name) {
      found = at;
      return false;
    }
    ++at;
    return true;
  });
  return found;
}

std::optional<std::string> ViewAxisSet::pointName(const ViewAxisId axis) const {
  const auto slot = slotAt(axis);
  return slot.has_value() ? textOf(*slot) : std::nullopt;
}

gleditor::cpp26::optional<const AxisRole &>
ViewAxisSet::role(const ViewAxisId axis) const {
  const auto slot = slotAt(axis);
  if (!slot.has_value()) {
    return {};
  }
  const auto roleCell = next(*slot, frame_->roles);
  const auto id       = roleCell.has_value() ? textOf(*roleCell) : std::nullopt;
  return id.has_value() ? roleNamed(*id)
                        : gleditor::cpp26::optional<const AxisRole &>{};
}

// -- groups ------------------------------------------------------------------

std::expected<BindTarget, ViewError>
ViewAxisSet::createGroup(const std::string_view name,
                         const std::span<const BindTarget> members) {
  const auto frame = ensureFrame();
  if (!frame) {
    return std::unexpected{frame.error()};
  }
  std::vector<BindTarget> resolved;
  resolved.reserve(members.size());
  for (const auto member : members) {
    const auto target = bindable(member);
    if (!target.has_value()) {
      return std::unexpected{ViewError::UnknownTarget};
    }
    resolved.push_back(*target);
  }
  const auto &f = **frame;
  std::optional<BindTarget> group;
  Edit edit{*this};
  const auto outcome = [&]() -> std::expected<void, ViewError> {
    const auto made = space_->mint(Layer::Binding, name);
    if (!made) {
      return std::unexpected{made.error()};
    }
    group = made->ref;
    if (auto done =
            edit.link(tailOf(f.groupsHead, f.groups), f.groups, pos, made->ref);
        !done) {
      return done;
    }
    auto tail = made->ref;
    for (const auto member : resolved) {
      const auto occurrence = space_->mintOccurrence(Layer::Binding, member);
      if (!occurrence) {
        return std::unexpected{occurrence.error()};
      }
      if (auto done = edit.link(tail, f.members, pos, occurrence->ref); !done) {
        return done;
      }
      tail = occurrence->ref;
    }
    return {};
  }();
  if (const auto done = finish(edit, outcome); !done) {
    return std::unexpected{done.error()};
  }
  return *group;
}

AxisResult ViewAxisSet::renameGroup(const BindTarget group,
                                    const std::string_view name) {
  if (!isGroup(group)) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  Edit edit{*this};
  return finish(edit, edit.text(group, name));
}

AxisResult ViewAxisSet::insertMember(const BindTarget group,
                                     const std::size_t position,
                                     const BindTarget member) {
  if (!isGroup(group)) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  const auto target = bindable(member);
  if (!target.has_value()) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  if (position > memberCount(group)) {
    return std::unexpected{ViewError::UnknownPlace};
  }
  if (isGroup(*target) && (*target == group || reaches(*target, group))) {
    return std::unexpected{ViewError::GroupCycle};
  }
  const auto members = frame_->members;
  const auto anchor  = 0 == position ? std::optional{group}
                                     : rankAt(group, members, position - 1);
  Edit edit{*this};
  const auto outcome = [&]() -> std::expected<void, ViewError> {
    const auto occurrence = space_->mintOccurrence(Layer::Binding, *target);
    if (!occurrence) {
      return std::unexpected{occurrence.error()};
    }
    return edit.insertAfter(*anchor, members, occurrence->ref);
  }();
  return finish(edit, outcome);
}

AxisResult ViewAxisSet::removeMember(const BindTarget group,
                                     const std::size_t position) {
  if (!isGroup(group)) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  const auto occurrence = rankAt(group, frame_->members, position);
  if (!occurrence.has_value()) {
    return std::unexpected{ViewError::UnknownPlace};
  }
  Edit edit{*this};
  return finish(edit, edit.detach(*occurrence, frame_->members));
}

AxisResult ViewAxisSet::moveMember(const BindTarget group,
                                   const std::size_t from,
                                   const std::size_t to) {
  if (!isGroup(group)) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  const auto count = memberCount(group);
  if (from >= count || to >= count) {
    return std::unexpected{ViewError::UnknownPlace};
  }
  if (from == to) {
    return this;
  }
  const auto members    = frame_->members;
  const auto occurrence = *rankAt(group, members, from);
  Edit edit{*this};
  const auto outcome = [&]() -> std::expected<void, ViewError> {
    if (auto done = edit.detach(occurrence, members); !done) {
      return done;
    }
    const auto anchor =
        0 == to ? std::optional{group} : rankAt(group, members, to - 1);
    return edit.insertAfter(*anchor, members, occurrence);
  }();
  return finish(edit, outcome);
}

AxisResult ViewAxisSet::deleteGroup(const BindTarget group) {
  if (!isGroup(group)) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  const auto &f = *frame_;
  // Collected before anything changes, so no walk reads a rank mid-edit.
  std::vector<zigzag::CellRef> slots;
  walk(f.axesHead, f.axes, [&](const zigzag::CellRef slot) {
    const auto occurrence = next(slot, f.binds);
    if (occurrence.has_value() && targetOf(*occurrence) == group) {
      slots.push_back(slot);
    }
    return true;
  });
  std::vector<zigzag::CellRef> uses;
  walk(f.groupsHead, f.groups, [&](const zigzag::CellRef parent) {
    walk(parent, f.members, [&](const zigzag::CellRef occurrence) {
      if (targetOf(occurrence) == group) {
        uses.push_back(occurrence);
      }
      return true;
    });
    return true;
  });
  // Its own member rank goes too: a rank headed by a group that is gone has
  // no meaning, and undo puts every link back.
  std::vector<zigzag::CellRef> chain{group};
  walk(group, f.members, [&](const zigzag::CellRef occurrence) {
    chain.push_back(occurrence);
    return true;
  });
  Edit edit{*this};
  const auto outcome = [&]() -> std::expected<void, ViewError> {
    for (const auto slot : slots) {
      if (auto done = edit.unlink(slot, f.binds, pos); !done) {
        return done;
      }
    }
    for (const auto occurrence : uses) {
      if (auto done = edit.detach(occurrence, f.members); !done) {
        return done;
      }
    }
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
      if (auto done = edit.unlink(chain[i], f.members, pos); !done) {
        return done;
      }
    }
    return edit.detach(group, f.groups);
  }();
  return finish(edit, outcome);
}

std::size_t ViewAxisSet::memberCount(const BindTarget group) const noexcept {
  return isGroup(group) ? rankSize(group, frame_->members) : 0;
}

std::optional<BindTarget>
ViewAxisSet::member(const BindTarget group,
                    const std::size_t position) const noexcept {
  if (!isGroup(group)) {
    return std::nullopt;
  }
  const auto occurrence = rankAt(group, frame_->members, position);
  return occurrence.has_value() ? targetOf(*occurrence) : std::nullopt;
}

std::optional<std::string>
ViewAxisSet::groupName(const BindTarget group) const {
  return isGroup(group) ? textOf(group) : std::nullopt;
}

void ViewAxisSet::forEachGroup(
    gleditor::cpp26::function_ref<void(BindTarget)> visit) const {
  if (!frame_.has_value()) {
    return;
  }
  walk(frame_->groupsHead, frame_->groups, [&](const zigzag::CellRef group) {
    visit(group);
    return true;
  });
}

// -- ring order
// ----------------------------------------------------------------

std::expected<std::size_t, ViewError>
ViewAxisSet::ringPlace(const zigzag::DimRef dimension) {
  const auto dim = realDimension(dimension);
  if (!dim.has_value()) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  const auto frame = ensureFrame();
  if (!frame) {
    return std::unexpected{frame.error()};
  }
  const auto &f = **frame;
  if (const auto found = findOccurrence(f.ringHead, f.ring, *dim)) {
    return found->place;
  }
  const auto place      = rankSize(f.ringHead, f.ring);
  const auto occurrence = space_->mintOccurrence(Layer::Binding, *dim);
  if (!occurrence) {
    return std::unexpected{occurrence.error()};
  }
  if (!space_->link(Layer::Binding, cell(tailOf(f.ringHead, f.ring)),
                    cell(f.ring), pos, *occurrence)) {
    return std::unexpected{ViewError::ArenaRefused};
  }
  return place;
}

std::optional<std::size_t>
ViewAxisSet::ringPlaceIfKnown(const zigzag::DimRef dimension) const {
  const auto dim = realDimension(dimension);
  if (!dim.has_value()) {
    return std::nullopt;
  }
  if (frame_.has_value()) {
    const auto found = findOccurrence(frame_->ringHead, frame_->ring, *dim);
    return found.has_value() ? std::optional{found->place} : std::nullopt;
  }
  // Not minted yet: the order it will begin as.
  const auto dims  = space_->base().dimensions();
  const auto found = std::ranges::find(dims, *dim);
  return static_cast<std::size_t>(found - dims.begin());
}

std::expected<void, ViewError>
ViewAxisSet::relinkRing(const zigzag::DimRef dimension,
                        const std::size_t place) {
  const auto &f    = *frame_;
  const auto found = findOccurrence(f.ringHead, f.ring, dimension);
  if (!found.has_value()) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  Edit raw{*this, false};
  if (auto done = raw.detach(found->cell, f.ring); !done) {
    return done;
  }
  const auto anchor = 0 == place ? std::optional{f.ringHead}
                                 : rankAt(f.ringHead, f.ring, place - 1);
  if (!anchor.has_value()) {
    return std::unexpected{ViewError::UnknownPlace};
  }
  return raw.insertAfter(*anchor, f.ring, found->cell);
}

AxisResult ViewAxisSet::moveInRing(const zigzag::DimRef dimension,
                                   const std::size_t place) {
  const auto dim = realDimension(dimension);
  if (!dim.has_value()) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  const auto current = ringPlace(*dim);
  if (!current) {
    return std::unexpected{current.error()};
  }
  if (place >= rankSize(frame_->ringHead, frame_->ring)) {
    return std::unexpected{ViewError::UnknownPlace};
  }
  if (*current == place) {
    return this;
  }
  Edit edit{*this};
  const auto outcome = relinkRing(*dim, place);
  if (outcome) {
    edit.ring(RingStep{.dimension = *dim, .from = *current, .to = place});
  }
  return finish(edit, outcome);
}

void ViewAxisSet::forEachInRing(
    gleditor::cpp26::function_ref<void(zigzag::DimRef)> visit) const {
  if (!frame_.has_value()) {
    for (const auto dim : space_->base().dimensions()) {
      visit(dim);
    }
    return;
  }
  walk(frame_->ringHead, frame_->ring, [&](const zigzag::CellRef occurrence) {
    if (const auto dim = targetOf(occurrence); dim.has_value()) {
      visit(*dim);
    }
    return true;
  });
}

// -- the pouch
// -------------------------------------------------------------------

AxisResult ViewAxisSet::addToPouch(const zigzag::DimRef dimension) {
  const auto dim = realDimension(dimension);
  if (!dim.has_value()) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  const auto frame = ensureFrame();
  if (!frame) {
    return std::unexpected{frame.error()};
  }
  const auto &f = **frame;
  if (findOccurrence(f.pouchHead, f.pouch, *dim).has_value()) {
    return this;
  }
  Edit edit{*this};
  const auto outcome = [&]() -> std::expected<void, ViewError> {
    const auto occurrence = space_->mintOccurrence(Layer::Binding, *dim);
    if (!occurrence) {
      return std::unexpected{occurrence.error()};
    }
    return edit.link(tailOf(f.pouchHead, f.pouch), f.pouch, pos,
                     occurrence->ref);
  }();
  return finish(edit, outcome);
}

AxisResult ViewAxisSet::removeFromPouch(const zigzag::DimRef dimension) {
  const auto dim = realDimension(dimension);
  if (!dim.has_value()) {
    return std::unexpected{ViewError::UnknownTarget};
  }
  if (!frame_.has_value()) {
    return this;
  }
  const auto found = findOccurrence(frame_->pouchHead, frame_->pouch, *dim);
  if (!found.has_value()) {
    return this;
  }
  Edit edit{*this};
  return finish(edit, edit.detach(found->cell, frame_->pouch));
}

bool ViewAxisSet::inPouch(const zigzag::DimRef dimension) const noexcept {
  const auto dim = realDimension(dimension);
  return dim.has_value() && frame_.has_value() &&
         findOccurrence(frame_->pouchHead, frame_->pouch, *dim).has_value();
}

void ViewAxisSet::forEachInPouch(
    gleditor::cpp26::function_ref<void(zigzag::DimRef)> visit) const {
  if (!frame_.has_value()) {
    return;
  }
  walk(frame_->pouchHead, frame_->pouch, [&](const zigzag::CellRef occurrence) {
    if (const auto dim = targetOf(occurrence); dim.has_value()) {
      visit(*dim);
    }
    return true;
  });
}

// -- who uses what -----------------------------------------------------------

void ViewAxisSet::forEachAxisShowing(
    const BindTarget target,
    gleditor::cpp26::function_ref<void(ViewAxisId)> visit) const {
  if (!frame_.has_value()) {
    return;
  }
  const auto wanted = realDimension(target).value_or(target);
  ViewAxisId at     = 0;
  walk(frame_->axesHead, frame_->axes, [&](const zigzag::CellRef slot) {
    const auto occurrence = next(slot, frame_->binds);
    if (occurrence.has_value() && targetOf(*occurrence) == wanted) {
      visit(at);
    }
    ++at;
    return true;
  });
}

void ViewAxisSet::forEachGroupContaining(
    const BindTarget target,
    gleditor::cpp26::function_ref<void(BindTarget)> visit) const {
  if (!frame_.has_value()) {
    return;
  }
  const auto wanted = realDimension(target).value_or(target);
  forEachGroup([&](const BindTarget group) {
    bool contains = false;
    walk(group, frame_->members, [&](const zigzag::CellRef occurrence) {
      contains = targetOf(occurrence) == wanted;
      return !contains;
    });
    if (contains) {
      visit(group);
    }
  });
}

// -- the verifier's share ----------------------------------------------------

std::size_t ViewAxisSet::verify(
    gleditor::cpp26::function_ref<void(const ViewSpaceViolation &)> report)
    const {
  if (!frame_.has_value()) {
    return 0;
  }
  const auto &f     = *frame_;
  const auto &arena = space_->arena(Layer::Binding);
  const auto cells  = arena.cells();
  std::size_t count = 0;
  const auto flag   = [&](const zigzag::CellRef ref,
                        const std::string_view rule) {
    report(ViewSpaceViolation{
          .cell = cell(ref), .layer = Layer::Binding, .rule = rule});
    ++count;
  };
  const auto isOccurrence = [&](const zigzag::CellRef ref) {
    return arena.handleTarget(ref).has_value();
  };
  const auto showsBindable = [&](const zigzag::CellRef ref) {
    const auto target = arena.handleTarget(ref);
    return target.has_value() && bindable(*target).has_value();
  };
  const auto showsDimension = [&](const zigzag::CellRef ref) {
    const auto target = arena.handleTarget(ref);
    return target.has_value() && realDimension(*target).has_value();
  };

  // Every cell on @p dim must be reached from one of @p heads and pass
  // @p fits; @p longest bounds how many follow each head, if anything does.
  const auto rank = [&](const std::span<const zigzag::CellRef> heads,
                        const zigzag::CellRef dim, const std::string_view rule,
                        const auto &fits,
                        const std::optional<std::size_t> longest) {
    std::vector<bool> reached(cells.size(), false);
    for (const auto head : heads) {
      if (const auto dense = arena.denseOf(head)) {
        reached[*dense] = true;
      }
      std::size_t length = 0;
      for (auto cur = arena.linked(head, dim, pos); cur.has_value();
           cur      = arena.linked(*cur, dim, pos)) {
        const auto dense = arena.denseOf(*cur);
        if (!dense.has_value() || reached[*dense]) {
          break;
        }
        reached[*dense] = true;
        ++length;
        if (!fits(*cur) || (longest.has_value() && length > *longest)) {
          flag(*cur, rule);
        }
      }
    }
    for (std::size_t dense = 0; dense < cells.size(); ++dense) {
      const auto ref = cells[dense].birthOp;
      if (reached[dense] || !zigzag::isEphemeral(ref)) {
        continue;
      }
      if (arena.linked(ref, dim, pos).has_value() ||
          arena.linked(ref, dim, neg).has_value()) {
        flag(ref, rule);
      }
    }
  };
  const auto notOccurrence = [&](const zigzag::CellRef ref) {
    return !isOccurrence(ref);
  };
  const auto namesRole = [&](const zigzag::CellRef ref) {
    return !isOccurrence(ref) && roleNamed(arena.textOf(ref)).has_value();
  };

  rank(std::array{f.axesHead}, f.axes, "axes-shape", notOccurrence,
       std::nullopt);
  rank(std::array{f.groupsHead}, f.groups, "groups-shape", notOccurrence,
       std::nullopt);
  rank(std::array{f.ringHead}, f.ring, "ring-shape", showsDimension,
       std::nullopt);
  rank(std::array{f.pouchHead}, f.pouch, "pouch-shape", showsDimension,
       std::nullopt);

  std::vector<zigzag::CellRef> slots;
  walk(f.axesHead, f.axes, [&](const zigzag::CellRef slot) {
    slots.push_back(slot);
    return true;
  });
  rank(slots, f.binds, "binds-shape", showsBindable, 1);
  rank(slots, f.roles, "role-shape", namesRole, 1);

  std::vector<zigzag::CellRef> groups;
  forEachGroup([&](const BindTarget group) { groups.push_back(group); });
  rank(groups, f.members, "dim-group-shape", showsBindable, std::nullopt);
  for (const auto group : groups) {
    if (reaches(group, group)) {
      flag(group, "group-cycle");
    }
  }
  return count;
}

// -- persistence by name
// -------------------------------------------------------

SavedBindings saveBindings(const ViewManifold &space,
                           const xanadu::SpanReader &reader) {
  const auto &axes = space.axes();
  const auto &base = space.base();
  SavedBindings saved;
  std::vector<BindTarget> groups;
  axes.forEachGroup([&](const BindTarget group) { groups.push_back(group); });
  const auto savedOf = [&](const BindTarget target) -> SavedTarget {
    if (axes.isGroup(target)) {
      return SavedGroupRef{
          .index = static_cast<std::size_t>(std::ranges::find(groups, target) -
                                            groups.begin())};
    }
    return SavedDimension{.name = base.textOf(target, reader)};
  };
  for (ViewAxisId axis = 0; axis < axes.axisCount(); ++axis) {
    const auto shows = axes.shown(axis);
    saved.axes.push_back(SavedAxis{
        .point = axes.pointName(axis).value_or(std::string{}),
        .shows =
            shows.has_value() ? std::optional{savedOf(*shows)} : std::nullopt,
    });
  }
  for (const auto group : groups) {
    SavedGroup entry{.name    = axes.groupName(group).value_or(std::string{}),
                     .members = {}};
    for (std::size_t i = 0; i < axes.memberCount(group); ++i) {
      if (const auto member = axes.member(group, i); member.has_value()) {
        entry.members.push_back(savedOf(*member));
      }
    }
    saved.groups.push_back(std::move(entry));
  }
  axes.forEachInRing([&](const zigzag::DimRef dim) {
    saved.ringOrder.push_back(base.textOf(dim, reader));
  });
  axes.forEachInPouch([&](const zigzag::DimRef dim) {
    saved.pouch.push_back(base.textOf(dim, reader));
  });
  return saved;
}

namespace {

/// The saved groups in an order that makes every member group before the
/// group that holds it (G9), and the members that would close a cycle.
struct GroupOrder {
  std::vector<std::size_t> order;
  std::set<std::pair<std::size_t, std::size_t>> cyclic; // (group, member)
};

GroupOrder leavesFirst(const std::vector<SavedGroup> &groups) {
  enum class State : std::uint8_t { Unseen, Open, Done };
  GroupOrder result;
  std::vector<State> state(groups.size(), State::Unseen);
  struct Cursor {
    std::size_t group, member;
  };
  for (std::size_t root = 0; root < groups.size(); ++root) {
    if (State::Unseen != state[root]) {
      continue;
    }
    state[root] = State::Open;
    std::vector<Cursor> stack{{.group = root, .member = 0}};
    while (!stack.empty()) {
      const auto group    = stack.back().group;
      const auto &members = groups[group].members;
      if (stack.back().member == members.size()) {
        state[group] = State::Done;
        result.order.push_back(group);
        stack.pop_back();
        continue;
      }
      const auto position   = stack.back().member++;
      const auto *const ref = std::get_if<SavedGroupRef>(&members[position]);
      if (ref == nullptr || ref->index >= groups.size()) {
        continue;
      }
      if (State::Open == state[ref->index]) {
        result.cyclic.emplace(group, position);
      } else if (State::Unseen == state[ref->index]) {
        state[ref->index] = State::Open;
        stack.push_back({.group = ref->index, .member = 0});
      }
    }
  }
  return result;
}

} // namespace

ReplayReport replayBindings(ViewManifold &space, const SavedBindings &saved,
                            const xanadu::SpanReader &reader) {
  auto &axes       = space.axes();
  const auto &base = space.base();
  ReplayReport report;
  const auto dimension = [&](const std::string &name) {
    return base.dimensionNamed(name, reader);
  };
  const auto notice = [&](const ViewMessage message, std::string name,
                          std::string where) {
    report.unresolved.push_back(ReplayNotice{.message = message,
                                             .name    = std::move(name),
                                             .where   = std::move(where)});
  };
  const auto groupLabel = [&](const std::size_t index) {
    return index < saved.groups.size() ? saved.groups[index].name
                                       : "group " + std::to_string(index);
  };

  const auto [order, cyclic] = leavesFirst(saved.groups);
  std::vector<std::optional<BindTarget>> made(saved.groups.size());
  for (const auto index : order) {
    const auto &group = saved.groups[index];
    std::vector<BindTarget> members;
    for (std::size_t position = 0; position < group.members.size();
         ++position) {
      const auto &entry = group.members[position];
      if (const auto *const dim = std::get_if<SavedDimension>(&entry)) {
        if (const auto found = dimension(dim->name)) {
          members.push_back(*found);
        } else {
          notice(ViewMessage::SavedMemberGone, dim->name, group.name);
        }
        continue;
      }
      const auto inner = std::get<SavedGroupRef>(entry).index;
      if (cyclic.contains({index, position})) {
        notice(ViewMessage::GroupInsideItself, groupLabel(inner), group.name);
      } else if (inner < made.size() && made[inner].has_value()) {
        members.push_back(*made[inner]);
      } else {
        notice(ViewMessage::SavedMemberGone, groupLabel(inner), group.name);
      }
    }
    if (const auto created = axes.createGroup(group.name, members)) {
      made[index] = *created;
      ++report.restored;
    }
  }

  for (const auto &axis : saved.axes) {
    const auto id = axes.axisNamed(axis.point);
    if (!id.has_value()) {
      notice(ViewMessage::SavedNameGone, axis.point, "the binding points");
      continue;
    }
    if (!axis.shows.has_value()) {
      continue;
    }
    std::optional<BindTarget> target;
    std::string name;
    if (const auto *const dim = std::get_if<SavedDimension>(&*axis.shows)) {
      name   = dim->name;
      target = dimension(dim->name);
    } else {
      const auto index = std::get<SavedGroupRef>(*axis.shows).index;
      name             = groupLabel(index);
      target           = index < made.size() ? made[index] : std::nullopt;
    }
    if (!target.has_value()) {
      notice(ViewMessage::SavedBindingGone, name, axis.point);
      continue;
    }
    if (const auto bound = axes.bind(*id, *target); bound) {
      ++report.restored;
    } else if (ViewError::EmptyGroupBind == bound.error()) {
      notice(ViewMessage::EmptyGroup, name, axis.point);
    }
  }

  std::size_t place = 0;
  for (const auto &name : saved.ringOrder) {
    const auto dim = dimension(name);
    if (!dim.has_value()) {
      notice(ViewMessage::SavedNameGone, name, "the ring order");
    } else if (axes.moveInRing(*dim, place)) {
      ++place;
    }
  }
  for (const auto &name : saved.pouch) {
    const auto dim = dimension(name);
    if (!dim.has_value()) {
      notice(ViewMessage::SavedNameGone, name, "the pouch");
    } else {
      std::ignore = axes.addToPouch(*dim);
    }
  }
  axes.forgetHistory();
  GLEDITOR_LOG_DEBUG(
      "view.binding",
      "replayed {} bindings and groups; {} saved names unresolved",
      report.restored, report.unresolved.size());
  return report;
}

} // namespace xanadu::view

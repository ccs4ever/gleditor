/**
 * @file lens_file_picker.cpp
 * @brief Implementation of the lens-backed ZigZag file-picker.
 */
#include "common/ui/lens_file_picker.hpp"

#include <deque>
#include <unordered_set>
#include <utility>

#include <gleditor/logging.hpp>

#include "common/xanadu/zigzag/filesystem_lens.hpp"
#include "common/xanadu/zigzag/projection_to_zz.hpp"

namespace xanadu::ui {

namespace {

/**
 * A FilesystemLens that drops files failing a predicate before projection.
 * Filtering inside the lens -- rather than hiding cells in the emitted
 * document -- keeps the projected rank topology honest: no orphan attribute
 * cells, no seq chains bridging invisible entries. Directories are always
 * kept so every kept file remains reachable.
 *
 * The structural edges are re-synthesized, not merely filtered: a dropped
 * middle sibling would otherwise orphan every later sibling (only the first
 * child has a d.contains edge; the rest chain on d.seq), so the kept
 * children of each directory are re-chained in their original order.
 */
class FilteredFilesystemLens final : public zigzag::ProjectionLens {
public:
  FilteredFilesystemLens(std::filesystem::path root,
                         LensFilePicker::Filter filter)
      : root_(std::move(root)), inner_(root_), filter_(std::move(filter)) {
    prepare();
  }

  [[nodiscard]] std::vector<
      std::pair<std::string, std::optional<zigzag::GenericDim>>>
  dimensions() const override {
    return inner_.dimensions();
  }

  void forEachNode(
      gleditor::cpp26::function_ref<void(const zigzag::SourceNode &)> emit)
      const override {
    inner_.forEachNode([this, &emit](const zigzag::SourceNode &node) {
      if (keep_.contains(node.key)) {
        emit(node);
      }
    });
  }

  void forEachEdge(
      gleditor::cpp26::function_ref<void(const zigzag::SourceEdge &)> emit)
      const override {
    // Sibling chains were captured in the source's emission order; iterate
    // parents in that same order for a deterministic edge stream.
    for (const auto &parent : parentOrder_) {
      const auto it = keptChildren_.find(parent);
      if (it == keptChildren_.end()) {
        continue;
      }
      const auto &kids = it->second;
      for (std::size_t i = 0; i < kids.size(); ++i) {
        if (i == 0) {
          emit(zigzag::SourceEdge{
              .from      = parent,
              .to        = kids[0],
              .dimension = std::string(
                  zigzag::genericDimName(zigzag::GenericDim::Containment)),
              .dir = zigzag::Posward});
        } else {
          emit(zigzag::SourceEdge{
              .from      = kids[i - 1],
              .to        = kids[i],
              .dimension = std::string(
                  zigzag::genericDimName(zigzag::GenericDim::Sequence)),
              .dir = zigzag::Posward});
        }
      }
    }
    for (const auto &edge : keptEdges_) {
      emit(edge);
    }
  }

private:
  void prepare() {
    // 1. Collect node metadata: display name (content) and role (attribute).
    struct Info {
      std::string name;
      std::string role;
    };
    std::unordered_map<zigzag::ExternalKey, Info> infos;
    std::vector<zigzag::ExternalKey> order;
    inner_.forEachNode([&](const zigzag::SourceNode &node) {
      Info info;
      if (const auto *s = std::get_if<std::string>(&node.content)) {
        info.name = *s;
      }
      for (const auto &[rank, value] : node.attributes) {
        if (rank == "d.role") {
          info.role = value;
        }
      }
      order.push_back(node.key);
      infos.emplace(node.key, std::move(info));
    });

    // 2. Read the structural edges back into parent/child tables. The inner
    //    lens emits parent -> first-child on d.contains and sibling ->
    //    sibling on d.seq; non-structural edges (d.ref) pass through.
    std::unordered_map<zigzag::ExternalKey, zigzag::ExternalKey> firstChild,
        seqNext;
    inner_.forEachEdge([&](const zigzag::SourceEdge &edge) {
      if (edge.dimension ==
          zigzag::genericDimName(zigzag::GenericDim::Containment)) {
        firstChild[edge.from] = edge.to;
      } else if (edge.dimension ==
                 zigzag::genericDimName(zigzag::GenericDim::Sequence)) {
        seqNext[edge.from] = edge.to;
      } else {
        otherEdges_.push_back(edge);
      }
    });

    std::unordered_map<zigzag::ExternalKey, std::vector<zigzag::ExternalKey>>
        children;
    std::unordered_map<zigzag::ExternalKey, zigzag::ExternalKey> parentOf;
    for (const auto &[parent, head] : firstChild) {
      auto &kids = children[parent];
      for (auto cur = head;;) {
        kids.push_back(cur);
        parentOf[cur] = parent;
        const auto it = seqNext.find(cur);
        if (it == seqNext.end()) {
          break;
        }
        cur = it->second;
      }
    }
    // Emit parents in the source's node order, not unordered_map order.
    for (const auto &key : order) {
      if (children.contains(key)) {
        parentOrder_.push_back(key);
      }
    }

    // 3. Resolve every entry's absolute path by walking down from the
    //    parentless root(s) (a FilesystemLens emits exactly one).
    std::unordered_map<zigzag::ExternalKey, std::filesystem::path> pathOf;
    std::deque<zigzag::ExternalKey> queue;
    for (const auto &key : order) {
      if (!parentOf.contains(key)) {
        pathOf[key] = root_;
        queue.push_back(key);
      }
    }
    while (!queue.empty()) {
      const auto parent = queue.front();
      queue.pop_front();
      for (const auto &kid : children[parent]) {
        pathOf[kid] = pathOf[parent] / infos[kid].name;
        queue.push_back(kid);
      }
    }

    // 4. Keep roots, all directories, and files passing the filter.
    for (const auto &key : order) {
      const auto &info  = infos[key];
      const bool isRoot = !parentOf.contains(key);
      if (isRoot || info.role == "dir" || !filter_ || filter_(pathOf[key])) {
        keep_.insert(key);
      }
    }
    for (const auto &[parent, kids] : children) {
      for (const auto &kid : kids) {
        if (keep_.contains(kid)) {
          keptChildren_[parent].push_back(kid);
        }
      }
    }
    for (const auto &edge : otherEdges_) {
      if (keep_.contains(edge.from) && keep_.contains(edge.to)) {
        keptEdges_.push_back(edge);
      }
    }
  }

  std::filesystem::path root_;
  zigzag::FilesystemLens inner_;
  LensFilePicker::Filter filter_;
  std::unordered_set<zigzag::ExternalKey> keep_;
  std::vector<zigzag::SourceEdge> otherEdges_, keptEdges_;
  std::unordered_map<zigzag::ExternalKey, std::vector<zigzag::ExternalKey>>
      keptChildren_;
  std::vector<zigzag::ExternalKey> parentOrder_;
};

} // namespace

LensFilePicker::LensFilePicker(std::filesystem::path root, Filter filter)
    : root_(std::move(root)), filter_(std::move(filter)) {
  label_ = root_.string();
  project();
}

LensFilePicker::~LensFilePicker() = default;

LensFilePicker *LensFilePicker::setDocumentSink(DocumentSink sink) {
  sink_ = std::move(sink);
  emitDocument();
  return this;
}

LensFilePicker *LensFilePicker::setFilter(Filter filter) {
  filter_ = std::move(filter);
  project();
  return this;
}

LensFilePicker *LensFilePicker::open() {
  activate();
  markDirty();
  return this;
}

void LensFilePicker::project() {
  session_ = std::make_unique<zigzag::ProjectionSession>(
      std::make_unique<FilteredFilesystemLens>(root_, filter_), core_);
  // Resolve the dims through the core, exactly as the session minted them --
  // findOrMintDimension is idempotent, so this never creates a second cell.
  dContains_ = core_.findOrMintDimension("d.contains");
  dSeq_      = core_.findOrMintDimension("d.seq");
  dRole_     = core_.findOrMintDimension("d.role");
  rebuildItems();
  if (!session_->roots().empty()) {
    cursor_.store(session_->roots().front());
  }
  markDirty();
}

void LensFilePicker::rebuildItems() {
  items_.clear();
  visible_.clear();

  // Iterative DFS in sibling order, mirroring FilesystemLens::walk -- a deep
  // directory tree must not land on the call stack.
  struct Frame {
    zigzag::CellRef cell;
    std::filesystem::path path;
  };
  std::vector<Frame> stack;
  for (auto it = session_->roots().rbegin(); it != session_->roots().rend();
       ++it) {
    stack.push_back({.cell = *it, .path = root_});
  }
  while (!stack.empty()) {
    const Frame frame = std::move(stack.back());
    stack.pop_back();

    const auto role = arena_.linked(frame.cell, dRole_, zigzag::Posward);
    items_.emplace(frame.cell, Item{.cell  = frame.cell,
                                    .path  = frame.path,
                                    .isDir = role.has_value() &&
                                             arena_.textOf(*role) == "dir"});
    visible_.push_back(frame.path);

    // Children: first on d.contains, the rest chained on d.seq.
    std::vector<zigzag::CellRef> kids;
    for (auto kid = firstChild(frame.cell); kid.has_value();
         kid      = nextSibling(*kid)) {
      kids.push_back(*kid);
    }
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
      stack.push_back({.cell = *it, .path = frame.path / arena_.textOf(*it)});
    }
  }
}

zigzag::OptionalCell
LensFilePicker::nextSibling(const zigzag::CellRef cell) const {
  return arena_.linked(cell, dSeq_, zigzag::Posward);
}

zigzag::OptionalCell
LensFilePicker::prevSibling(const zigzag::CellRef cell) const {
  return arena_.linked(cell, dSeq_, zigzag::Negward);
}

zigzag::OptionalCell
LensFilePicker::firstChild(const zigzag::CellRef cell) const {
  return arena_.linked(cell, dContains_, zigzag::Posward);
}

zigzag::OptionalCell
LensFilePicker::parentOf(const zigzag::CellRef cell) const {
  if (auto parent = arena_.linked(cell, dContains_, zigzag::Negward);
      parent.has_value()) {
    return parent;
  }
  for (auto sib = arena_.linked(cell, dSeq_, zigzag::Negward); sib.has_value();
       sib      = arena_.linked(*sib, dSeq_, zigzag::Negward)) {
    if (auto parent = arena_.linked(*sib, dContains_, zigzag::Negward);
        parent.has_value()) {
      return parent;
    }
  }
  return std::nullopt;
}

const LensFilePicker::Item *
LensFilePicker::item(const zigzag::CellRef cell) const {
  const auto it = items_.find(cell);
  return it == items_.end() ? nullptr : &it->second;
}

void LensFilePicker::moveCursor(const zigzag::CellRef cell) {
  if (items_.contains(cell)) {
    cursor_.store(cell);
    markDirty();
  }
}

void LensFilePicker::descend() {
  if (auto kid = firstChild(cursor_.load()); kid.has_value()) {
    moveCursor(*kid);
  }
}

void LensFilePicker::ascend() {
  if (auto parent = parentOf(cursor_.load()); parent.has_value()) {
    moveCursor(*parent);
  }
}

void LensFilePicker::activateCursor() {
  const Item *cur = item(cursor_.load());
  if (cur == nullptr) {
    return;
  }
  if (cur->isDir) {
    descend();
  } else {
    finishPick(cur->path);
  }
}

void LensFilePicker::finishPick(const std::filesystem::path &path) {
  pickedPath_ = path;
  finished_.store(true);
  deactivate();
  GLEDITOR_LOG_DEBUG("ui.filepicker", "picked {}", path.string());
  if (onPick_) {
    onPick_(path);
  }
}

void LensFilePicker::doCancel() {
  cancelled_.store(true);
  GLEDITOR_LOG_DEBUG("ui.filepicker", "cancelled under {}", root_.string());
  finished_.store(true);
  deactivate();
  if (onCancel_) {
    onCancel_();
  }
}

void LensFilePicker::cancel() { doCancel(); }

void LensFilePicker::markDirty() {
  docDirty_.store(true);
  // Push eagerly when a sink is bound: the picker must emit without a render
  // loop behind it, and drawFrame stays as the flush for a sink bound later.
  if (sink_) {
    emitDocument();
  }
}

void LensFilePicker::emitDocument() {
  zigzag::ZzStructureDocument doc;
  {
    const std::lock_guard lock(emitMutex_);
    docDirty_.store(false);
    if (session_ == nullptr) {
      return;
    }
    doc = buildDocument();
  }
  // The sink is the host's adoptDocument; run it outside the emission lock
  // so a host calling back into the picker cannot self-deadlock.
  if (sink_) {
    sink_(std::move(doc));
  }
}

zigzag::ZzStructureDocument LensFilePicker::buildDocument() {
  auto doc =
      zigzag::projectionToZz(label_.empty() ? std::string_view{root_.string()}
                                            : std::string_view{label_},
                             *session_, arena_);
  doc.focus = cursor_.load();
  return doc;
}

zigzag::ZzStructureDocument LensFilePicker::document() {
  const std::lock_guard lock(emitMutex_);
  return buildDocument();
}

std::optional<std::filesystem::path> LensFilePicker::cursorPath() const {
  if (const Item *cur = item(cursor_.load()); cur != nullptr) {
    return cur->path;
  }
  return std::nullopt;
}

bool LensFilePicker::keyPressed(const gleditor::Key key,
                                const gleditor::KeyMods /*mods*/) {
  if (!active() || finished_.load() || session_ == nullptr) {
    return false;
  }
  switch (key) {
  case gleditor::Key::Escape:
    doCancel();
    return true;
  case gleditor::Key::Up:
    if (auto prev = prevSibling(cursor_.load()); prev.has_value()) {
      moveCursor(*prev);
    } else if (auto parent = parentOf(cursor_.load()); parent.has_value()) {
      moveCursor(*parent);
    }
    return true;
  case gleditor::Key::Down:
    if (auto next = nextSibling(cursor_.load()); next.has_value()) {
      moveCursor(*next);
    }
    return true;
  case gleditor::Key::Right:
    descend();
    return true;
  case gleditor::Key::Left:
  case gleditor::Key::Backspace:
    ascend();
    return true;
  case gleditor::Key::Return:
    activateCursor();
    return true;
  default:
    return false;
  }
}

void LensFilePicker::drawFrame(gleditor::FrameContext & /*ctx*/) {
  if (docDirty_.load()) {
    emitDocument();
  }
}

} // namespace xanadu::ui

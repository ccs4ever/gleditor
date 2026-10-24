/**
 * @file lens_file_picker.hpp
 * @brief A reusable ZigZag file-picker: a directory tree projected through
 *        the FilesystemLens into a navigable slice, resolved to a path.
 *
 * The picker enumerates a start directory with zigzag::FilesystemLens
 * (recursively; the lens is a bounded ProjectionLens), projects it through
 * zigzag::ProjectionSession, and hands the host a ZzStructureDocument built by
 * zigzag::projectionToZz -- the same document ZigzagVisualizer::adoptDocument
 * renders, with d.contains on X and d.seq on Y, so directories and files
 * appear as a navigable containment tree. The host binds the DocumentSink to
 * its slice view's adoptDocument; the picker pushes a fresh document whenever
 * the cursor moves.
 *
 * Input comes through the same seam every other overlay uses: the picker is
 * a gleditor::ui::FocusScope, so a FocusManager (or a direct caller) feeds it
 * KeyEvents. Up/Down walk siblings on d.seq (Up off the first sibling lands
 * on the parent), Right/Return descend into a directory, Left/Backspace climb
 * to the parent, Return on a file fires onPick(path), Ctrl+Return picks the
 * entry under the cursor even when it is a directory (the "open this
 * directory" gesture a lens host needs), Escape fires onCancel.
 *
 * A Filter predicate set at construction (or via setFilter before open())
 * decides which files appear and are pickable -- e.g. an extension check for
 * ".sqlite"/".json"/".log". Directories are always listed so every kept file
 * stays reachable.
 *
 * The class is self-contained: it owns its arena, VortexCore and session,
 * needs no render device, and never touches the filesystem after the initial
 * enumeration. Construct it, bind the callbacks, open() it, feed it keys.
 */
#ifndef COMMON_UI_LENS_FILE_PICKER_HPP
#define COMMON_UI_LENS_FILE_PICKER_HPP

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gleditor/frame_contributor.hpp>
#include <gleditor/ui/focus_manager.hpp>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"

namespace xanadu::ui {

class LensFilePicker final : public gleditor::FrameContributor,
                             public gleditor::ui::FocusScope {
public:
  /// Whether a file path is listed and pickable. Directories are always
  /// listed (the tree must stay navigable); an empty filter keeps every file.
  using Filter = std::function<bool(const std::filesystem::path &)>;
  /// Called once, with the picked file's absolute path, on Return over a
  /// file cell.
  using PickHandler = std::function<void(const std::filesystem::path &)>;
  /// Called once on Escape/cancel().
  using CancelHandler = std::function<void()>;
  /// The host's slice-adoption seam: bind ZigzagVisualizer::adoptDocument
  /// (capturing the view and the source label it wants). The picker calls it
  /// with a freshly built document each time the cursor moves.
  using DocumentSink = std::function<void(zigzag::ZzStructureDocument &&)>;

  /// Enumerates @p root immediately (recursive, bounded); the listing is a
  /// snapshot. @p filter gates files only; directories are always kept.
  explicit LensFilePicker(std::filesystem::path root, Filter filter = {});
  ~LensFilePicker() override;

  LensFilePicker(const LensFilePicker &)            = delete;
  LensFilePicker &operator=(const LensFilePicker &) = delete;
  LensFilePicker(LensFilePicker &&)                 = delete;
  LensFilePicker &operator=(LensFilePicker &&)      = delete;

  LensFilePicker *setOnPick(PickHandler cb) {
    onPick_ = std::move(cb);
    return this;
  }
  LensFilePicker *setOnCancel(CancelHandler cb) {
    onCancel_ = std::move(cb);
    return this;
  }
  /// Binding the sink also pushes the initial document, so a host that sets
  /// it before open() sees the listing as soon as the picker activates.
  LensFilePicker *setDocumentSink(DocumentSink sink);
  LensFilePicker *setLabel(std::string label) {
    label_ = std::move(label);
    return this;
  }
  /// Re-enumerate with a different filter. Setup-phase only: call before
  /// open() (the session is rebuilt wholesale, so calling it while a render
  /// thread reads document() is a data race).
  LensFilePicker *setFilter(Filter filter);

  /// Activate the scope and mark the document for emission. The host
  /// registers the picker with its FocusManager beforehand.
  LensFilePicker *open();

  // -- gleditor::ui::FocusScope ----------------------------------------------
  bool keyPressed(const gleditor::ui::KeyEvent &event) override {
    return keyPressed(event.key, event.mods);
  }
  bool keyPressed(gleditor::Key key, gleditor::KeyMods mods);
  /// Escape lands here through FocusManager: finish with onCancel.
  void cancel() override;

  // -- gleditor::FrameContributor --------------------------------------------
  /// Flushes a pending document to the sink. Draws nothing itself -- the
  /// slice view the sink adopts into does the rendering.
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override {
    return docDirty_.load() && sink_ != nullptr;
  }

  // -- outcome ----------------------------------------------------------------
  /// True after a pick or a cancel; the picker consumes no keys afterwards.
  [[nodiscard]] bool finished() const { return finished_.load(); }
  [[nodiscard]] bool cancelled() const { return cancelled_.load(); }
  /// The picked path, when finished() && !cancelled().
  [[nodiscard]] const std::optional<std::filesystem::path> &
  pickedPath() const noexcept {
    return pickedPath_;
  }

  // -- inspection (the test seam)
  // ----------------------------------------------
  /// Absolute path under the cursor, if the cursor is on an entry.
  [[nodiscard]] std::optional<std::filesystem::path> cursorPath() const;
  /// The cursor's cell, for hosts that would rather navigateFocusTo() than
  /// re-adopt a whole document.
  [[nodiscard]] zigzag::CellRef cursorCell() const noexcept {
    return cursor_.load();
  }
  /// Every listed entry's path in depth-first display order (filtered).
  [[nodiscard]] const std::vector<std::filesystem::path> &
  visiblePaths() const noexcept {
    return visible_;
  }
  /// The document the sink receives, built on demand: the projected tree
  /// with focus on the cursor cell.
  [[nodiscard]] zigzag::ZzStructureDocument document();
  /// The live projection, for hosts that drive session verbs.
  [[nodiscard]] zigzag::ProjectionSession &session() { return *session_; }
  /// Typed text narrows the listing to basenames containing it;
  /// Backspace edits the filter while one is typed, else ascends.
  void textTyped(std::string_view utf8) override;
  /// The active text filter, for the host's status line or a11y.
  [[nodiscard]] const std::string &filterText() const noexcept {
    return filterText_;
  }

private:
  struct Item {
    zigzag::CellRef cell{};
    std::filesystem::path path;
    bool isDir{false};
  };

  void project();
  void rebuildItems();

  [[nodiscard]] zigzag::OptionalCell nextSibling(zigzag::CellRef cell) const;
  [[nodiscard]] zigzag::OptionalCell prevSibling(zigzag::CellRef cell) const;
  [[nodiscard]] zigzag::OptionalCell firstChild(zigzag::CellRef cell) const;
  /// A non-first sibling has no d.contains edge of its own: its parent is the
  /// contains-negward neighbour of the sibling chain's head.
  [[nodiscard]] zigzag::OptionalCell parentOf(zigzag::CellRef cell) const;

  [[nodiscard]] const Item *item(zigzag::CellRef cell) const;
  void moveCursor(zigzag::CellRef cell);
  void descend();
  void ascend();
  void activateCursor();
  void finishPick(const std::filesystem::path &path);
  void doCancel();
  void markDirty();
  void emitDocument();
  /// Caller holds emitMutex_.
  [[nodiscard]] zigzag::ZzStructureDocument buildDocument();

  std::filesystem::path root_;
  Filter filter_;
  /// Typed text filter over entry basenames; composes with filter_.
  std::string filterText_;
  std::string label_;
  PickHandler onPick_;
  CancelHandler onCancel_;
  DocumentSink sink_;

  zigzag::ArenaManifold arena_;
  zigzag::vortex::VortexCore core_{arena_};
  std::unique_ptr<zigzag::ProjectionSession> session_;
  zigzag::DimRef dContains_{};
  zigzag::DimRef dSeq_{};
  zigzag::DimRef dRole_{};
  std::unordered_map<zigzag::CellRef, Item> items_;
  /// Listed paths in depth-first display order, filled by rebuildItems().
  std::vector<std::filesystem::path> visible_;
  std::atomic<zigzag::CellRef> cursor_{};
  std::atomic<bool> finished_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> docDirty_{false};
  std::optional<std::filesystem::path> pickedPath_;
  /// Serializes snapshot builds: keyPressed runs on the event thread,
  /// drawFrame on the render thread.
  std::mutex emitMutex_;
};

} // namespace xanadu::ui

#endif // COMMON_UI_LENS_FILE_PICKER_HPP

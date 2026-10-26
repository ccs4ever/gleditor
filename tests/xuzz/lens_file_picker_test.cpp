/**
 * @file lens_file_picker_test.cpp
 * @brief Tests for xanadu::ui::LensFilePicker -- the reusable lens-backed
 *        ZigZag file-picker (apps/common/ui/lens_file_picker.hpp).
 *
 * A fixture directory is projected through the picker's filtered
 * FilesystemLens; the tests drive the pick through the FocusScope key path
 * (the same seam FocusManager dispatches on) and assert the path that comes
 * out, the listing the filter leaves, and cancellation.
 */
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "common/ui/lens_file_picker.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::ui::LensFilePicker;

// A deterministic fixture:
//   root/
//     config.json
//     drop.txt        (filtered out)
//     keep.sqlite
//     logs/
//       app.log
//       debug.txt     (filtered out)
//     sub/
//       data.sqlite
struct Fixture {
  fs::path root;

  Fixture() {
    root = fs::temp_directory_path() /
           ("lens_pick_fix_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "logs");
    fs::create_directories(root / "sub");
    auto write = [](const fs::path &p) {
      std::ofstream f(p);
      f << "x\n";
    };
    write(root / "config.json");
    write(root / "drop.txt");
    write(root / "keep.sqlite");
    write(root / "logs" / "app.log");
    write(root / "logs" / "debug.txt");
    write(root / "sub" / "data.sqlite");
  }
  ~Fixture() { fs::remove_all(root); }

  /// The .sqlite/.json/.log-style extension restriction call sites bind.
  static bool wanted(const fs::path &p) {
    const auto ext = p.extension().string();
    return ext == ".sqlite" || ext == ".json" || ext == ".log";
  }
};

bool press(LensFilePicker &picker, const gleditor::Key key) {
  return picker.keyPressed(key, gleditor::KeyMods::None);
}

TEST(LensFilePickerTest, CtrlReturnPicksADirectory) {
  Fixture fix;
  std::optional<fs::path> picked;
  LensFilePicker picker(fix.root, Fixture::wanted);
  picker.setOnPick([&](const fs::path &p) { picked = p; });
  picker.open();

  // The cursor opens on the root directory itself.
  EXPECT_TRUE(
      picker.keyPressed(gleditor::Key::Return, gleditor::KeyMods::Ctrl));
  ASSERT_TRUE(picked.has_value());
  EXPECT_EQ(*picked, fix.root);
}

TEST(LensFilePickerTest, CtrlReturnOnNestedDirPicksIt) {
  Fixture fix;
  std::optional<fs::path> picked;
  LensFilePicker picker(fix.root, Fixture::wanted);
  picker.setOnPick([&](const fs::path &p) { picked = p; });
  picker.open();

  EXPECT_TRUE(press(picker, gleditor::Key::Right)); // -> config.json
  EXPECT_TRUE(press(picker, gleditor::Key::Down));  // -> keep.sqlite
  EXPECT_TRUE(press(picker, gleditor::Key::Down));  // -> logs/
  EXPECT_TRUE(
      picker.keyPressed(gleditor::Key::Return, gleditor::KeyMods::Ctrl));
  ASSERT_TRUE(picked.has_value());
  EXPECT_EQ(*picked, fix.root / "logs");
}

TEST(LensFilePickerTest, FilterLeavesDirsAndMatchingFiles) {
  Fixture fix;
  LensFilePicker picker(fix.root, Fixture::wanted);

  const std::vector<fs::path> expected{
      fix.root,
      fix.root / "config.json",
      fix.root / "keep.sqlite",
      fix.root / "logs",
      fix.root / "logs" / "app.log",
      fix.root / "sub",
      fix.root / "sub" / "data.sqlite",
  };
  EXPECT_EQ(picker.visiblePaths(), expected);
}

TEST(LensFilePickerTest, UnfilteredListsEverything) {
  Fixture fix;
  LensFilePicker picker(fix.root);
  // root + 5 children + 2 in logs + 1 in sub = 9.
  EXPECT_EQ(picker.visiblePaths().size(), 9U);
}

TEST(LensFilePickerTest, ReturnOnFilePicksItsPath) {
  Fixture fix;
  std::optional<fs::path> picked;
  LensFilePicker picker(fix.root, Fixture::wanted);
  picker.setOnPick([&](const fs::path &p) { picked = p; });
  picker.open();

  // The cursor starts on the root directory; Right descends onto the first
  // kept child (sorted order: config.json comes first).
  EXPECT_TRUE(press(picker, gleditor::Key::Right));
  EXPECT_EQ(picker.cursorPath(), fix.root / "config.json");
  EXPECT_TRUE(press(picker, gleditor::Key::Return));

  ASSERT_TRUE(picked.has_value());
  EXPECT_EQ(*picked, fix.root / "config.json");
  EXPECT_TRUE(picker.finished());
  EXPECT_FALSE(picker.cancelled());
  ASSERT_TRUE(picker.pickedPath().has_value());
  EXPECT_EQ(*picker.pickedPath(), fix.root / "config.json");
}

TEST(LensFilePickerTest, NavigateIntoDirectoryAndPickNestedFile) {
  Fixture fix;
  std::optional<fs::path> picked;
  LensFilePicker picker(fix.root, Fixture::wanted);
  picker.setOnPick([&](const fs::path &p) { picked = p; });
  picker.open();

  EXPECT_TRUE(press(picker, gleditor::Key::Right)); // -> config.json
  EXPECT_TRUE(press(picker, gleditor::Key::Down));  // -> keep.sqlite
  EXPECT_EQ(picker.cursorPath(), fix.root / "keep.sqlite");
  EXPECT_TRUE(press(picker, gleditor::Key::Down)); // -> logs/
  EXPECT_EQ(picker.cursorPath(), fix.root / "logs");
  EXPECT_TRUE(press(picker, gleditor::Key::Right)); // -> logs/app.log
  EXPECT_EQ(picker.cursorPath(), fix.root / "logs" / "app.log");
  // debug.txt was filtered out: app.log is logs' only kept child, so Down
  // stays put.
  EXPECT_TRUE(press(picker, gleditor::Key::Down));
  EXPECT_EQ(picker.cursorPath(), fix.root / "logs" / "app.log");
  EXPECT_TRUE(press(picker, gleditor::Key::Return));

  ASSERT_TRUE(picked.has_value());
  EXPECT_EQ(*picked, fix.root / "logs" / "app.log");
}

TEST(LensFilePickerTest, LeftClimbsToParentAndUpClimbsPastFirstSibling) {
  Fixture fix;
  LensFilePicker picker(fix.root, Fixture::wanted);
  picker.open();

  EXPECT_TRUE(press(picker, gleditor::Key::Right)); // -> config.json
  EXPECT_TRUE(press(picker, gleditor::Key::Down));  // -> keep.sqlite
  EXPECT_TRUE(press(picker, gleditor::Key::Down));  // -> logs/
  EXPECT_TRUE(press(picker, gleditor::Key::Down));  // -> sub/
  EXPECT_EQ(picker.cursorPath(), fix.root / "sub");
  EXPECT_TRUE(press(picker, gleditor::Key::Right)); // -> sub/data.sqlite
  // Left on a first-and-only child still reaches the parent directory.
  EXPECT_TRUE(press(picker, gleditor::Key::Left));
  EXPECT_EQ(picker.cursorPath(), fix.root / "sub");
  // Up on the first sibling (config.json) lands on its parent, the root.
  EXPECT_TRUE(press(picker, gleditor::Key::Left));  // -> root
  EXPECT_TRUE(press(picker, gleditor::Key::Right)); // -> config.json
  EXPECT_TRUE(press(picker, gleditor::Key::Up));
  EXPECT_EQ(picker.cursorPath(), fix.root);
  // Up/Left at the root are inert but still consumed.
  EXPECT_TRUE(press(picker, gleditor::Key::Up));
  EXPECT_TRUE(press(picker, gleditor::Key::Left));
  EXPECT_EQ(picker.cursorPath(), fix.root);
}

TEST(LensFilePickerTest, EscapeCancelsThroughTheFocusScopeCancel) {
  Fixture fix;
  bool cancelled = false;
  LensFilePicker picker(fix.root, Fixture::wanted);
  picker.setOnCancel([&] { cancelled = true; });
  picker.open();

  EXPECT_TRUE(press(picker, gleditor::Key::Escape));

  EXPECT_TRUE(cancelled);
  EXPECT_TRUE(picker.cancelled());
  EXPECT_TRUE(picker.finished());
  EXPECT_FALSE(picker.pickedPath().has_value());
  // A finished picker consumes no further keys.
  EXPECT_FALSE(press(picker, gleditor::Key::Down));
}

// FocusManager's cancel() path (focus lost, outside pointer) lands the same
// way as Escape.
TEST(LensFilePickerTest, ScopeCancelAlsoCancels) {
  Fixture fix;
  bool cancelled = false;
  LensFilePicker picker(fix.root, Fixture::wanted);
  picker.setOnCancel([&] { cancelled = true; });
  picker.open();
  picker.cancel();
  EXPECT_TRUE(cancelled);
  EXPECT_TRUE(picker.finished());
}

TEST(LensFilePickerTest, DocumentFocusFollowsTheCursor) {
  Fixture fix;
  LensFilePicker picker(fix.root, Fixture::wanted);
  picker.open();

  EXPECT_TRUE(press(picker, gleditor::Key::Right)); // -> config.json
  EXPECT_TRUE(press(picker, gleditor::Key::Down));  // -> keep.sqlite

  auto doc = picker.document();
  ASSERT_TRUE(doc.focus);
  EXPECT_EQ(doc.focus, picker.cursorCell());
  const auto it = doc.cells.find(*doc.focus);
  ASSERT_NE(it, doc.cells.end());
  EXPECT_EQ(it->second.text(), "keep.sqlite");
  // The slice binds containment on X and sibling order on Y, the same view
  // LensSurfaceContributor adopts.
  EXPECT_EQ(doc.view.x_dimension, "d.contains");
  EXPECT_EQ(doc.view.y_dimension, "d.seq");
}

TEST(LensFilePickerTest, DocumentSinkReceivesSnapshotsOnCursorMoves) {
  Fixture fix;
  std::vector<std::optional<zigzag::CellID>> focuses;
  LensFilePicker picker(fix.root, Fixture::wanted);
  picker.setDocumentSink(
      [&](zigzag::ZzStructureDocument &&doc) { focuses.push_back(doc.focus); });
  picker.open();

  EXPECT_TRUE(press(picker, gleditor::Key::Right));
  EXPECT_TRUE(press(picker, gleditor::Key::Down));

  // The initial adoption plus each cursor move. The exact count depends on
  // flush timing, but the last document's focus must be the cursor cell.
  ASSERT_FALSE(focuses.empty());
  EXPECT_EQ(focuses.back(), picker.cursorCell());
}

TEST(LensFilePickerTest, ReturnOnEmptyDirectoryIsInert) {
  const fs::path root = fs::temp_directory_path() /
                        ("lens_pick_empty_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "empty");

  bool picked = false;
  LensFilePicker picker(root, Fixture::wanted);
  picker.setOnPick([&](const fs::path &) { picked = true; });
  picker.open();

  EXPECT_TRUE(press(picker, gleditor::Key::Right));  // -> empty/
  EXPECT_TRUE(press(picker, gleditor::Key::Return)); // descend into nothing
  EXPECT_EQ(picker.cursorPath(), root / "empty");
  EXPECT_FALSE(picked);
  EXPECT_FALSE(picker.finished());

  fs::remove_all(root);
}

} // namespace

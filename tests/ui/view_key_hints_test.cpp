/**
 * @file view_key_hints_test.cpp
 * @brief The hint line under a slice, built from the keymap (M2 finding B5):
 *        a view presentation advertises the keys it runs and none it refuses.
 */
#include <gtest/gtest.h>

#include <string>

#include <SDL.h>

#include <gleditor/app.hpp>

#include "common/ui/slice/zigzag_commands.hpp"
#include "common/xanadu/system_docs.hpp"

namespace {

namespace settings = xanadu::settings;
using gleditor::Mod;

gleditor::CommandTable bound() {
  gleditor::CommandTable table;
  const auto act = [&table](const std::string_view name, const int scancode,
                            const Mod mods) {
    table.bind(scancode, mods, std::string(name), "action", [] {});
  };
  act(settings::kKeymapStepXPos, SDL_SCANCODE_RIGHT, Mod::None);
  act(settings::kKeymapStepXNeg, SDL_SCANCODE_LEFT, Mod::None);
  act(settings::kKeymapJumpHome, SDL_SCANCODE_HOME, Mod::None);
  act(settings::kKeymapHopHead, SDL_SCANCODE_A, Mod::None);
  act(settings::kKeymapViewPalette, SDL_SCANCODE_V, Mod::Ctrl | Mod::Alt);
  act(settings::kKeymapViewBind, SDL_SCANCODE_B, Mod::Ctrl | Mod::Alt);
  act(settings::kKeymapViewSubviewNext, SDL_SCANCODE_PERIOD,
      Mod::Ctrl | Mod::Alt);
  act(settings::kKeymapEditCell, SDL_SCANCODE_E, Mod::None);
  act(settings::kKeymapMarkCell, SDL_SCANCODE_M, Mod::None);
  return table;
}

} // namespace

TEST(ViewKeyHintsTest, AViewAdvertisesItsOwnKeysFromTheKeymap) {
  const auto table = bound();
  const auto hints = zigzag::viewKeyHints(table, {});
  const auto &here = hints.first;
  EXPECT_NE(here.find("Step: Left/Right"), std::string::npos) << here;
  EXPECT_NE(here.find("Home: Home"), std::string::npos) << here;
  EXPECT_NE(here.find("Hop: A"), std::string::npos) << here;
  EXPECT_NE(here.find("Views: Ctrl+Alt+V"), std::string::npos) << here;
  EXPECT_NE(here.find("Bind: Ctrl+Alt+B"), std::string::npos) << here;
  EXPECT_NE(here.find("Sub-view: Ctrl+Alt+."), std::string::npos) << here;
  // The view refuses editing, so it does not advertise it.
  EXPECT_EQ(here.find("Edit"), std::string::npos) << here;
  EXPECT_EQ(here.find("Mark"), std::string::npos) << here;
  // Two rows: movement, then the view commands.
  EXPECT_NE(here.find('\n'), std::string::npos) << here;
}

TEST(ViewKeyHintsTest, TheVisualizerKeepsItsEditingKeys) {
  const auto table = bound();
  const auto hints = zigzag::zigzagKeyHints(table, {});
  const auto &here = hints.first;
  EXPECT_NE(here.find("Edit: E"), std::string::npos) << here;
  EXPECT_NE(here.find("Mark: M"), std::string::npos) << here;
  EXPECT_EQ(here.find("Bind"), std::string::npos) << here;
}

TEST(ViewKeyHintsTest, AKeyThatIsNotBoundIsNotAdvertised) {
  const gleditor::CommandTable table;
  const auto hints = zigzag::viewKeyHints(table, {});
  EXPECT_TRUE(hints.first.empty()) << hints.first;
  EXPECT_TRUE(hints.second.empty()) << hints.second;
}

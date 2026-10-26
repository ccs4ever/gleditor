#include <gtest/gtest.h>

#include <gleditor/a11y/publisher.hpp>
#include <gleditor/form.hpp>

#include "common/ui/walks.hpp"

namespace {
std::vector<xanadu::WalkEntry> entries() {
  xanadu::DocumentId document;
  return {{.visit = {.id     = {1},
                     .target = xanadu::DocumentSite{document, {}, {0, 4}}}},
          {.visit      = {.id     = {2},
                          .parent = {{1}},
                          .target = xanadu::CellSite{document, {}, 8, {0, 3}}},
           .referenced = true},
          {.visit       = {.id     = {3},
                           .parent = {{1}},
                           .target = xanadu::CellSite{document, {}, 92, {0, 3}}},
           .annotations = {"Café second branch"}}};
}
std::string describe(const xanadu::OccurrenceSite &site) {
  if (const auto *cell = std::get_if<xanadu::CellSite>(&site))
    return "cell " + std::to_string(cell->cell);
  return "source document";
}
} // namespace

TEST(WalksFormTest, KeyboardPreviewChoosesEitherSavedFutureWithoutEntering) {
  gleditor::Form form;
  std::vector<xanadu::WalksAction> actions;
  xanadu::openWalksForm(
      form, entries(), xanadu::VisitId{1}, describe,
      [&](auto action) { actions.push_back(std::move(action)); });
  const auto fields = form.current();
  ASSERT_EQ(fields.size(), 3U);
  EXPECT_TRUE(fields[0].options[0].starts_with("Root visit 1"));
  EXPECT_TRUE(fields[0].options[1].starts_with("Branch visit 2 from 1"));
  EXPECT_TRUE(fields[0].optionDescriptions[1].contains("saved reference"));
  EXPECT_TRUE(fields[0].optionDescriptions[2].contains("Café second branch"));
  form.keyPressed(gleditor::Key::Right, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Right, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Tab, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Tab, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  ASSERT_EQ(actions.size(), 1U);
  EXPECT_EQ(actions.front().kind, xanadu::WalksActionKind::Preview);
  EXPECT_EQ(actions.front().visit, xanadu::VisitId{3});
  xanadu::openWalksForm(
      form, entries(), xanadu::VisitId{2}, describe,
      [&](auto action) { actions.push_back(std::move(action)); });
  form.keyPressed(gleditor::Key::Tab, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Tab, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  ASSERT_EQ(actions.size(), 2U);
  EXPECT_EQ(actions.back().visit, xanadu::VisitId{2});
  EXPECT_EQ(actions.back().kind, xanadu::WalksActionKind::Preview);
}

TEST(WalksFormTest, AccessibilityFieldsPreserveTargetAndAnnotationIdentity) {
  using gleditor::a11y::Action;
  gleditor::Form form;
  std::optional<xanadu::WalksAction> chosen;
  xanadu::openWalksForm(form, entries(), xanadu::VisitId{1}, describe,
                        [&](auto action) { chosen = std::move(action); });
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder into(tree, 16);
  form.describe(into);
  EXPECT_EQ(tree.find(into.id(16))->label, "Saved visit");
  EXPECT_EQ(tree.find(into.id(80))->label, "Action");
  EXPECT_EQ(tree.find(into.id(144))->label, "Annotation");
  EXPECT_TRUE(form.performAction(into.id(19), Action::Click, {}));
  EXPECT_TRUE(form.performAction(into.id(84), Action::Click, {}));
  EXPECT_TRUE(form.performAction(into.id(144), Action::SetValue,
                                 "Café note\nsecond line"));
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->visit, xanadu::VisitId{3});
  EXPECT_EQ(chosen->kind, xanadu::WalksActionKind::Annotate);
  EXPECT_EQ(chosen->annotation, "Café note\nsecond line");
}

TEST(WalksFormTest, EmptyWalkCannotInventAVisit) {
  gleditor::Form form;
  int accepted{};
  xanadu::openWalksForm(form, {}, std::nullopt, describe,
                        [&](auto) { ++accepted; });
  form.keyPressed(gleditor::Key::Tab, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Tab, gleditor::KeyMods::None);
  form.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);
  EXPECT_EQ(accepted, 0);
}

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/view.hpp"

namespace {

using xanadu::view::ChordSpec;
using xanadu::view::SubviewSpec;
using xanadu::view::View;
using xanadu::view::ViewDescriptor;
using xanadu::view::ViewError;
using xanadu::view::ViewRegistry;
using xanadu::view::ViewSubject;

class NamedView final : public View {
public:
  explicit NamedView(std::string kind) : kind_(std::move(kind)) {}
  [[nodiscard]] std::string_view kind() const noexcept override {
    return kind_;
  }

private:
  std::string kind_;
};

ViewDescriptor descriptor(std::string kind,
                          std::vector<ChordSpec> chords = {}) {
  ViewDescriptor d;
  d.kind    = kind;
  d.name    = "A test view";
  d.subject = ViewSubject::Slice;
  d.chords  = std::move(chords);
  d.make    = [kind] { return std::make_unique<NamedView>(kind); };
  return d;
}

ChordSpec chord(std::string call, std::string keys, std::string context = {}) {
  return {.action  = call,
          .call    = std::move(call),
          .chord   = std::move(keys),
          .context = std::move(context)};
}

/// The first chord of the default keymap, with the scope it is live in.
std::pair<std::string, std::string> aDefaultChord() {
  const auto specs = xanadu::defaultSettingSpecs(xanadu::SystemDocKind::Keymap);
  const auto &spec = specs.front();
  return {std::get<std::string>(spec.schemas.front().defaultValues.front()),
          std::string(xanadu::keymapScope(spec.name))};
}

} // namespace

TEST(ViewRegistryTest, OneCallInstallsAViewOfAnyKind) {
  ViewRegistry registry;
  auto stretch     = descriptor("slice.stretch-vanishing");
  stretch.subviews = {SubviewSpec{.id = "plain", .name = "Plain"},
                      SubviewSpec{.id = "heat", .name = "Edge heat"}};
  ASSERT_TRUE(registry.add(std::move(stretch)).and_then([](ViewRegistry *r) {
    return r->add(descriptor("acme.third-party"));
  }));

  ASSERT_EQ(registry.views().size(), 2U);
  EXPECT_EQ(registry.views()[1].kind, "acme.third-party");
  const auto found = registry.find("slice.stretch-vanishing");
  ASSERT_TRUE(found);
  ASSERT_EQ(found->subviews.size(), 2U);
  EXPECT_EQ(found->subviews.front().id, "plain");
  EXPECT_EQ(found->make()->kind(), "slice.stretch-vanishing");
  EXPECT_FALSE(registry.find("slice.unknown"));
}

TEST(ViewRegistryTest, ASecondViewOfOneKindIsRefused) {
  ViewRegistry registry;
  ASSERT_TRUE(registry.add(descriptor("page.base")));
  auto again =
      descriptor("page.base", {chord("std:view/again", "Ctrl+Alt+F11")});
  EXPECT_EQ(registry.add(std::move(again)).error(),
            ViewError::DuplicateViewKind);
  EXPECT_EQ(registry.views().size(), 1U);
  // The refused descriptor's chord was not taken.
  EXPECT_FALSE(registry.chordHolder("Ctrl+Alt+F11", ""));
}

TEST(ViewRegistryTest, AChordTakenInItsScopeIsRefusedHoweverItIsSpelled) {
  ViewRegistry registry;
  ASSERT_TRUE(registry.add(descriptor(
      "slice.first", {chord("std:view/first", "Ctrl+Alt+F11", "zigzag")})));

  auto second = descriptor(
      "slice.second", {chord("std:view/fine", "Ctrl+Alt+F12", "zigzag"),
                       chord("std:view/second", "alt+ctrl+f11", "zigzag")});
  EXPECT_EQ(registry.add(std::move(second)).error(), ViewError::ChordCollision);
  EXPECT_FALSE(registry.find("slice.second"));
  EXPECT_FALSE(registry.chordHolder("Ctrl+Alt+F12", "zigzag"))
      << "a refused descriptor reserved one of its chords";
  EXPECT_EQ(registry.chordHolder("Alt+Ctrl+F11", "zigzag"), "std:view/first");

  // The same chord in another scope is the point of scopes.
  EXPECT_TRUE(registry.add(descriptor(
      "page.other", {chord("std:view/other", "Ctrl+Alt+F11", "document")})));
}

TEST(ViewRegistryTest, AChordTheDefaultKeymapHoldsIsRefused) {
  ViewRegistry registry;
  const auto [keys, scope] = aDefaultChord();
  ASSERT_TRUE(registry.chordHolder(keys, scope));
  EXPECT_EQ(registry
                .add(descriptor("slice.greedy",
                                {chord("std:view/greedy", keys, scope)}))
                .error(),
            ViewError::ChordCollision);
}

TEST(ViewRegistryTest, OneViewCannotBindOneChordTwice) {
  ViewRegistry registry;
  EXPECT_EQ(
      registry
          .add(descriptor("slice.twice", {chord("std:view/a", "Ctrl+Alt+F11"),
                                          chord("std:view/b", "Ctrl+Alt+F11")}))
          .error(),
      ViewError::ChordCollision);
  EXPECT_TRUE(registry.views().empty());
}

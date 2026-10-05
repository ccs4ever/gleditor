#include <Page.h>
#include <gleditor/doc.hpp>
#include <gtest/gtest.h>

#include <type_traits>
#include <typeinfo>
#include <utility>

namespace {

// Static Poppler and the editor are linked together on distribution targets.
// Including both definitions here prevents a shared global Page identity.
static_assert(!std::is_same_v<gleditor::Page, ::Page>);
static_assert(std::is_base_of_v<Drawable, gleditor::Page>);
static_assert(
    std::is_same_v<decltype(std::declval<const Doc &>().page(0)),
                   gleditor::cpp26::optional<const gleditor::Page &>>);

TEST(PageNamespaceTest, EditorAndPopplerHaveSeparateRuntimeTypeIdentities) {
  EXPECT_NE(typeid(gleditor::Page), typeid(::Page));
}

} // namespace

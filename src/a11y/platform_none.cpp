/**
 * @file platform_none.cpp
 * @brief Accessibility on a machine that has no AccessKit installed.
 *
 * AccessKit is a separate library -- see the README on where to get it -- and
 * somebody building this project without it should still get a working editor.
 * So the platform end is an interface with two implementations and this is the
 * one that does nothing.
 *
 * Doing nothing means one particular thing: openPlatform() answers with
 * nothing, so no tree is ever sent. The tree is still built, every frame, by
 * the same code out of the same state, because the alternative -- describing
 * the user interface only in builds that have somewhere to send the
 * description -- is how a description rots. The cost of building it is a
 * comparison against the last one.
 *
 * Built when the Makefile cannot find AccessKit, or when
 * GLEDITOR_ENABLE_A11Y=0 says not to look. See platform_accesskit.cpp for the
 * real one.
 */
#include <gleditor/a11y/platform.hpp> // IWYU pragma: associated

namespace gleditor::a11y {

std::unique_ptr<Platform> openPlatform(void * /*nativeWindow*/,
                                       const std::string & /*name*/,
                                       const std::string & /*toolkit*/,
                                       const std::string & /*version*/) {
  return nullptr;
}

bool platformAvailable() { return false; }

} // namespace gleditor::a11y

/**
 * @file ncurses_actor.cpp
 * @brief Native actor implementation delegating to TerminalDevice.
 */
#include "common/xanadu/vagent/tui/ncurses_actor.hpp"

namespace xanadu::vagent::tui {

NCursesActor::NCursesActor() : NCursesActor(Options{}) {}

NCursesActor::NCursesActor(Options options)
    : options_(options), device_(xanadu::ui::tui::TerminalDevice::Options{
                             .forceHeadless = options.forceHeadless,
                             .defaultWidth  = options.defaultWidth,
                             .defaultHeight = options.defaultHeight}) {
  if (options.pid) {
    pid_ = *options.pid;
  } else {
    pid_ = identity::ActorPid{.node          = "local",
                              .cursor        = "tui/ncurses",
                              .epoch         = 1,
                              .spawnSequence = 1};
  }
}

NCursesActor::~NCursesActor() = default;

NCursesActor::NCursesActor(NCursesActor &&) noexcept            = default;
NCursesActor &NCursesActor::operator=(NCursesActor &&) noexcept = default;

} // namespace xanadu::vagent::tui

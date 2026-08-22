/**
 * @file pick_observer.hpp
 * @brief Being told that a click landed on something the library did not draw.
 *
 * A click is answered by the picking attachment, and the renderer knows how to
 * read three answers out of it: a page, a glyph, and an overlay. Everything a
 * program draws for itself through a FrameContributor writes that attachment
 * too -- that is the whole point of a canvas being able to set its own tag --
 * and until now the answer went nowhere. The renderer would look at a tag it
 * did not recognise, fail to resolve it to a character, and report the click
 * as unresolved.
 *
 * An observer is where those answers go. It is offered every completed pick
 * before the renderer does anything with it, and says whether it claimed it;
 * a claimed pick does not then move the caret, because a click on a program's
 * own drawing is a click on that drawing and not on the text behind it.
 */
#ifndef GLEDITOR_PICK_OBSERVER_H
#define GLEDITOR_PICK_OBSERVER_H

#include <gleditor/render/types.hpp>

struct RenderState;

namespace gleditor {

/**
 * @brief Offered every picking result, on the render thread.
 *
 * Observers are held as bare pointers and are not owned; one must outlive the
 * renderer or remove itself first.
 */
class PickObserver {
public:
  PickObserver()          = default;
  virtual ~PickObserver() = default;

  PickObserver(const PickObserver &)            = delete;
  PickObserver &operator=(const PickObserver &) = delete;
  PickObserver(PickObserver &&)                 = delete;
  PickObserver &operator=(PickObserver &&)      = delete;

  /**
   * @brief A click has been resolved to whatever was drawn at that pixel.
   *
   * @return True when this observer has dealt with it, which stops the
   *         renderer from placing the caret and stops any later observer from
   *         being offered it. An observer that does not recognise the tag must
   *         return false rather than swallowing every click in the window.
   */
  [[nodiscard]] virtual bool picked(const render::PickingResult &pick,
                                    RenderState &state) = 0;
};

} // namespace gleditor

#endif // GLEDITOR_PICK_OBSERVER_H

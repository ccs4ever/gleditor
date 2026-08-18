/**
 * @file animation.hpp
 * @brief The timings every animation in the editor shares, in one place.
 *
 * Choreograph drives the motion itself; what lives here is the vocabulary --
 * how long things take and how far a document travels -- so that the document
 * and the notification overlay agree rather than each inventing a duration.
 *
 * One timeline is stepped by the render loop, and everything animated hangs
 * off it. That is also what makes an animation observable: the loop can ask
 * whether anything is still moving, which is what a screenshot has to wait for
 * if it is to capture the finished result rather than the middle of a fade.
 */
#ifndef GLEDITOR_ANIMATION_H
#define GLEDITOR_ANIMATION_H

namespace gleditor::anim {

/**
 * @brief Seconds a document takes to reach its place and become opaque.
 *
 * Long enough to read as movement rather than a jump, short enough not to
 * delay a screenshot noticeably: the render loop waits for animations to
 * finish before calling a frame settled.
 */
inline constexpr double docArrival = 0.45;

/**
 * @brief How far in front of its resting place a document starts.
 *
 * Along +Z, which is towards the camera, so a document arrives by receding
 * into the row rather than sliding in from a side that a second document would
 * already be occupying.
 */
inline constexpr float docArrivalDepth = 40.0F;

/// Seconds a notification takes to fade in, and again to fade out.
inline constexpr double toastFade = 0.25;

} // namespace gleditor::anim

#endif // GLEDITOR_ANIMATION_H

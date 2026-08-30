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

/**
 * @brief Seconds the document a link brings alongside takes to get there.
 *
 * Longer than @ref docArrival on purpose. A sworph moves several things at
 * once -- the far document, the rest of the row making way for it, and the
 * camera closing around all of them -- and giving every one of them the same
 * duration reads as the whole view jumping rather than as one document being
 * brought over. This is the one the eye is meant to follow, so it is the one
 * that takes longest and leads.
 */
inline constexpr double sworphSubject = 0.62;

/// Seconds the rest of the foreground row takes to close up behind a sworph,
/// and how long it waits before starting. The delay is what makes the row read
/// as making way for the document rather than sliding alongside it.
inline constexpr double sworphRow      = 0.45;
inline constexpr double sworphRowDelay = 0.09;

/// Seconds the camera takes to settle on a new framing, and how long it waits
/// first. Last to move and slowest, so the frame closes around an arrangement
/// that has already been made rather than chasing one still being made.
inline constexpr double cameraSettle      = 0.70;
inline constexpr double cameraSettleDelay = 0.14;

/**
 * @brief Alpha a document opened behind the row settles at.
 *
 * Perspective alone is a weak depth cue for a flat page of text: a corpus
 * held far behind the row still draws every glyph at full strength, and a
 * reader looking for what just arrived has to pick it out of something just
 * as loud. Dimming what is behind is the cue that costs nothing to read.
 */
inline constexpr float backgroundOpacity = 0.42F;

/// Seconds a notification takes to fade in, and again to fade out.
inline constexpr double toastFade = 0.25;

} // namespace gleditor::anim

#endif // GLEDITOR_ANIMATION_H

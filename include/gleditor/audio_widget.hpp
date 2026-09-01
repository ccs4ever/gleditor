/**
 * @file audio_widget.hpp
 * @brief Compatibility header providing audio-specific type aliases for the
 *        media widget.
 */
#ifndef GLEDITOR_AUDIO_WIDGET_H
#define GLEDITOR_AUDIO_WIDGET_H

#include <gleditor/media_widget.hpp>

namespace gleditor {

using AudioWidget    = MediaWidget;
using AudioWidgetPtr = MediaWidgetPtr;

} // namespace gleditor

#endif // GLEDITOR_AUDIO_WIDGET_H

/**
 * @file audio.hpp
 * @brief Compatibility header providing audio-specific type aliases for the
 *        media subsystem.
 */
#ifndef GLEDITOR_AUDIO_H
#define GLEDITOR_AUDIO_H

#include <gleditor/media.hpp>

namespace gleditor {

using AudioMedia  = MediaResource;
using AudioPlayer = MediaPlayer;

} // namespace gleditor

#endif // GLEDITOR_AUDIO_H

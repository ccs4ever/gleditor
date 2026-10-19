#ifndef COMMON_XANADU_ANCHOR_MOTION_HPP
#define COMMON_XANADU_ANCHOR_MOTION_HPP

#include "common/xanadu/link_occurrences.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace xanadu {
struct AnchorFocus {
  LinkKey key;
  LinkSide side{};
  std::uint32_t member{}, occurrence{};
  DocumentSite site;
  bool operator==(const AnchorFocus &) const = default;
};
/// One current and one outgoing ornament, independent of page construction.
class AnchorMotion {
public:
  struct Track {
    std::optional<AnchorFocus> focus;
    float value{}, from{}, goal{};
    double began{};
  };
  void update(std::optional<AnchorFocus> focus, double now, float seconds,
              bool reduced) {
    for (auto &track : tracks_) {
      const auto t =
          seconds > 0 ? std::clamp((now - track.began) / seconds, 0., 1.) : 1.;
      track.value = reduced
                        ? track.goal
                        : std::lerp(track.from, track.goal,
                                    static_cast<float>(t * t * (3 - 2 * t)));
    }
    if (focus != chosen_) {
      chosen_ = focus;
      for (auto &track : tracks_) {
        track.from  = track.value;
        track.goal  = 0;
        track.began = now;
      }
      if (focus) {
        auto at = std::ranges::find(tracks_, focus, &Track::focus);
        if (at == tracks_.end()) {
          at  = std::ranges::min_element(tracks_, {}, &Track::value);
          *at = {.focus = focus, .began = now};
        }
        at->from  = at->value;
        at->goal  = 1;
        at->began = now;
      }
    }
    if (reduced || seconds == 0)
      for (auto &track : tracks_) {
        track.value = track.from = track.goal;
        track.began              = now;
      }
    moving_ = std::ranges::any_of(
        tracks_, [](const auto &track) { return track.value != track.goal; });
  }
  [[nodiscard]] bool moving() const { return moving_; }
  [[nodiscard]] const auto &tracks() const { return tracks_; }

private:
  std::array<Track, 2> tracks_{};
  std::optional<AnchorFocus> chosen_;
  bool moving_{};
};
} // namespace xanadu
#endif

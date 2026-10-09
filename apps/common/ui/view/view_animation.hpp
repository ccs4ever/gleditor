/**
 * @file view_animation.hpp
 * @brief Tweens between a placement's successive layouts, by SubjectId.
 *
 * design/view-system.md §8.9 and view-system-implementation-plan.md §5.3
 * (package U6a). Each time a placement is laid out again the host hands the
 * new records here; each frame it advances the clock and draws what comes
 * out. A subject in both layouts travels from where it is drawn now to its
 * new place; one that is new fades in; one that has gone fades out from the
 * copy kept of it, so nothing is ever looked up in a view space, a store or a
 * catalog. A derived view cell's id carries its epoch, so after a toss its
 * old id matches nothing and it fades in place while the real cells around it
 * travel.
 *
 * Under a held key every step is taken at once and retargets what is moving
 * from where it is, so nothing queues; a derived view cell that appears under
 * one is held back by one repeat interval before it fades in, so one that is
 * gone by the next repeat is never seen. With view.motion.reduced every change
 * is a cut.
 *
 * What comes out is flat: every record placed in placement-local space with
 * its frames already composed (placedPose), so a subject whose frame changed
 * or went away still follows one continuous path. Edges' labels are
 * renumbered to the items written.
 *
 * Easing is Choreograph's, as everywhere else in the program; its timeline is
 * not used, because it allocates for every motion it starts, and a held key
 * starts one per subject per repeat. The tracks here live in vectors that keep
 * their storage, so a placement whose subjects have stopped changing in number
 * allocates nothing to retarget or to advance. One instance serves one
 * placement on its owner thread.
 */
#ifndef COMMON_UI_VIEW_VIEW_ANIMATION_HPP
#define COMMON_UI_VIEW_VIEW_ANIMATION_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/view_ids.hpp"
#include "common/xanadu/view/view_records.hpp"

namespace xanadu::view {

/// Why a placement's records changed, which says how they move.
enum class MotionCause : std::uint8_t {
  /// A step or a page transition: subjects travel. A step that comes before
  /// the last one's motion would have ended is a held key's repeat. A toss is
  /// told by the epoch, not by the cause.
  Move,
  /// Bindings changed: subjects travel as for a step, but a rebinding is a
  /// command and never a key's repeat, so it holds nothing back.
  Rebind,
  /// The placement's view was replaced: the old scene fades out where it
  /// stands as the new one fades in, nothing travelling between them.
  ViewSwitch,
  /// Its sub-view changed: subjects travel, at sub-view speed.
  SubViewSwitch,
};

/// One new layout of a placement.
struct AnimationTarget {
  /// The records, and the MotionHints a view's transition() added to them.
  /// Read during retarget() only.
  const LayoutSink &layout;
  /// The view space's epoch the records were laid out in; 0 for a page view,
  /// which has none.
  ViewEpoch epoch{};
  MotionCause cause{MotionCause::Move};
};

class ViewAnimation {
public:
  using Millis = std::chrono::duration<double, std::milli>;

  explicit ViewAnimation(const MotionConfig &config = {});

  /// New settings apply from the next retarget; turning reduced motion on
  /// also cuts whatever is moving now.
  ViewAnimation *configure(const MotionConfig &config);
  [[nodiscard]] const MotionConfig &config() const noexcept { return config_; }

  /// The placement was laid out again: every subject heads for its new
  /// record from where it is drawn now.
  ViewAnimation *retarget(const AnimationTarget &target);

  /// Moves the clock on by @p dt and writes into @p out, which it clears
  /// first, what is to be drawn now.
  ViewAnimation *advance(Millis dt, LayoutSink &out);

  /// Whether anything is still moving or fading: a frame in which nothing is
  /// stale and this is false re-submits what it drew last (V-R44).
  [[nodiscard]] bool animating() const noexcept;

  /// Time since construction, as advance() has moved it.
  [[nodiscard]] Millis now() const noexcept { return Millis{nowMs_}; }

private:
  struct Timing {
    double startMs{};
    double durationMs{};
    MotionEase ease{MotionEase::Linear};
  };
  template <typename Record> struct Track {
    Record from{}, to{};
    Timing timing;
    bool leaving{};
    /// Items only: where the last advance() wrote it.
    std::optional<std::uint32_t> written;
    /// Edges only: the item track of the edge's label.
    std::optional<std::uint32_t> labelTrack;
  };
  struct IndexEntry {
    std::uint32_t track{};
    bool leaving{};
  };
  struct Change;

  template <typename Record, typename KeyLess>
  void retargetTracks(std::vector<Track<Record>> &tracks,
                      std::vector<Track<Record>> &next,
                      const std::vector<Record> &targets, const Change &change,
                      KeyLess less);
  [[nodiscard]] double progress(const Timing &timing) const noexcept;
  [[nodiscard]] bool running(const Timing &timing) const noexcept;
  template <typename Record>
  [[nodiscard]] Record sample(const Track<Record> &track) const noexcept;
  template <typename Record>
  [[nodiscard]] bool finished(const Track<Record> &track) const noexcept {
    return track.leaving && !running(track.timing);
  }
  void cut();

  MotionConfig config_;
  double nowMs_{};
  std::optional<ViewEpoch> epoch_;   // of the last layout; none before one
  std::optional<double> lastMoveMs_; // when the last step arrived

  std::vector<Track<PlacedItem>> items_, nextItems_;
  std::vector<Track<PlacedFrame>> frames_, nextFrames_;
  std::vector<Track<PlacedEdge>> edges_, nextEdges_;
  std::vector<DropTarget> dropTargets_;

  // Scratch, kept for its storage.
  std::vector<PlacedItem> targetItems_;
  std::vector<PlacedFrame> targetFrames_;
  std::vector<PlacedEdge> targetEdges_;
  std::vector<std::optional<std::uint32_t>> targetLabels_; // item tracks
  std::vector<std::optional<std::uint32_t>> trackOfSinkItem_;
  std::vector<MotionHint> hints_;
  std::vector<IndexEntry> index_;
  std::vector<bool> matched_;
};

} // namespace xanadu::view

#endif // COMMON_UI_VIEW_VIEW_ANIMATION_HPP

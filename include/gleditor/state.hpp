#ifndef GLEDITOR_STATE_H
#define GLEDITOR_STATE_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <glm/ext/vector_float3.hpp>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gleditor/modal_input.hpp>
#include <gleditor/render/diagnostics.hpp>

struct RenderItem;

struct AppState {
  /// Shared state between main and renderer threads
  // set before the render thread starts, no need to synchronize
  std::string defaultFontName;
  /// When set, the first fully drawn frame is written here as a PPM and the
  /// path is cleared. Used to compare backends pixel for pixel.
  std::string screenshotPath;
  /**
   * @brief One thing to do to the document once it has settled.
   *
   * The automation options are a script, carried out in the order they were
   * written on the command line rather than in categories. Order is the whole
   * point: "click here, type this, click there, type that" is a sequence, and
   * a run that did every click and then all the typing would be a different
   * test -- one that inserts everything at the last caret.
   *
   * Each step waits for the one before it. A click waits for its own picking
   * answer, and typing waits for the reflow it causes, so what the next step
   * sees is what the last one produced.
   */
  struct AutomationStep {
    enum class Kind : std::uint8_t {
      Pick,    ///< Report what is at a pixel.
      Click,   ///< Place the caret at a pixel.
      Type,    ///< Insert text at the caret, or into whatever has the keyboard.
      Select,  ///< Select a byte range of the first document.
      Command, ///< Run a bound command by name.
      Press,   ///< A key a modal takes: tab, enter, escape and friends.
    };
    Kind kind{};
    int x{}; ///< Pick and click: the pixel.
    int y{};
    std::uint32_t from{}; ///< Select: document-global byte offsets.
    std::uint32_t to{};
    std::string text; ///< Type: the UTF-8 to insert. Command: which command.
    gleditor::Key key{};      ///< Press: which key.
    gleditor::KeyMods mods{}; ///< Press: held with it.
  };
  /// The script, in command line order. Written before the render thread
  /// starts and only read after.
  std::vector<AutomationStep> script;

  /// Whether the script asks for any picking report, which is what decides
  /// whether hovering is reported: a run that named pixels wants those and not
  /// a line per frame about the mouse.
  [[nodiscard]] bool scriptReportsPicks() const {
    return std::ranges::any_of(script, [](const AutomationStep &step) {
      return AutomationStep::Kind::Pick == step.kind;
    });
  }
  std::atomic_bool alive{true};
  /// Set when the render thread stops because of an error rather than because
  /// it was asked to. The process exit status follows it, so a renderer that
  /// dies is not reported as a successful run.
  std::atomic_bool renderFailed{false};
  /// Notifications to show as soon as the first frame can be drawn. Driver
  /// diagnostics are the reason the overlay exists, and they cannot be provoked
  /// on demand, so this is how the overlay is exercised and compared between
  /// backends. Written before the render thread starts and only read after.
  std::vector<std::pair<render::DiagnosticSeverity, std::string>>
      requestedToasts;
  bool profiling{};
  /// Report where the data files were found and quit, without opening a
  /// window. The one thing a package can be asked on a machine whose GL driver
  /// cannot give it a context -- and the thing packaging most often gets wrong.
  bool printAssetDir{};
  /**
   * @brief Screen pixels per layout pixel below which a page is drawn as one
   *        solid bar per line rather than one quad per glyph.
   *
   * A glyph of this font stands around twenty layout pixels tall, so this is
   * about three screen pixels of glyph -- well past the point where the shapes
   * carry any information and comfortably below the default view, which gives a
   * glyph roughly eight. Zero draws every visible page in full detail, which is
   * what makes the two paths comparable.
   */
  float coarseBelow{0.15F};
  /// Whether pages outside the view are skipped. Off draws every page of every
  /// document, which is how the culled frame is checked against the unculled
  /// one.
  bool cullPages{true};
  /// Frames to draw, and time, once the document has settled, before quitting.
  /// Zero disables the measurement. Only settled frames are counted: a frame
  /// drawn while pages are still being built is measuring the loader, not the
  /// renderer.
  std::size_t benchmarkFrames{};
  /// When set, a driver error ends the render thread instead of being shown as
  /// a notification. Automated runs want it: a frame rendered by a driver that
  /// was reporting errors proves nothing, however plausible it looks.
  bool strictDiagnostics{};
  /// Draw frames but do not show them. See RenderDevice::setPresentEnabled.
  bool noPresent{};
  std::atomic<std::chrono::duration<float>> frameTimeDelta;
  std::atomic_int mouseX;
  std::atomic_int mouseY;
  /// Pixel of a click the render thread has not yet answered. Held as one
  /// pending position rather than a queue: a second click before the first is
  /// resolved supersedes it, which is what a user clicking twice means.
  std::atomic_int clickX{-1};
  std::atomic_int clickY{-1};
  std::atomic_bool clickPending{false};
  /// Pixel a drag has reached with the button still down. Answered like a
  /// click, but it extends the selection instead of replacing it.
  std::atomic_int dragX{-1};
  std::atomic_int dragY{-1};
  std::atomic_bool dragPending{false};
  /// Text typed since the render thread last drained it, in UTF-8. Guarded by
  /// its own mutex: SDL delivers it on the event thread and the edit is
  /// applied on the render thread.
  std::mutex typedMutex;
  std::string typedText;

  /**
   * @brief Whatever currently has the keyboard instead of the document.
   *
   * Null almost always. Set before the event loop starts and left alone: what
   * changes is whether it says it is grabbing, which is the modal's business
   * and is asked before every key. See gleditor/modal_input.hpp.
   */
  gleditor::ModalInput *modal{};

  /**
   * @brief Run a bound command by name, for a scripted run.
   *
   * Set by Application before the render thread starts. The commands live on
   * the event thread and every one of them queues its work, so calling one
   * from the render thread is the same as somebody pressing the key.
   */
  std::function<bool(const std::string &)> runCommand;
  struct ViewPerspective : public std::mutex {
    int screenWidth  = 800;
    int screenHeight = 600;
    float fov        = 5.0;
    glm::vec3 pos;
    glm::vec3 front;
    glm::vec3 upward;
    float speed = 60.0;
    ViewPerspective() { resetPos(); }
    void resetPos() {
      pos    = glm::vec3(0.0F, 0.0F, 1000.0F);
      front  = glm::vec3(0.0F, 0.0F, -1.0F);
      upward = glm::vec3(0.0F, 1.0F, 0.0F);
    }
  } view;
  ///
};

using AppStateRef = std::shared_ptr<AppState>;

#endif // GLEDITOR_STATE_H

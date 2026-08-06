#ifndef GLEDITOR_RENDERER_H
#define GLEDITOR_RENDERER_H

#include <concepts>
#include <functional>
#include <future>
#include <gleditor/tqueue.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gleditor/log.hpp>
// The full device declaration is needed here, not a forward declaration: the
// renderer holds a unique_ptr to one and create() instantiates the destructor
// in every translation unit that builds a renderer. The header names no
// graphics API, so this costs nothing in coupling.
#include <gleditor/caret.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/state.hpp>
#include <gleditor/toast.hpp>

struct AutoSDLWindow;
struct RenderState;

struct RenderItem {
  enum class Type : std::uint8_t {
    NewDoc,
    Resize,
    OpenDoc,
    Run,
  };
  Type type;

  explicit RenderItem(const Type type) : type(type) {}
  virtual ~RenderItem() = default;
};
struct RenderItemNewDoc : RenderItem {
  RenderItemNewDoc() : RenderItem(Type::NewDoc) {}
  ~RenderItemNewDoc() override = default;
};

struct RenderItemResize : RenderItem {
  int width, height;
  RenderItemResize(const int width, const int height)
      : RenderItem(Type::Resize), width(width), height(height) {}
  ~RenderItemResize() override = default;
};

struct RenderItemOpenDoc : RenderItem {
  std::string docFile;
  explicit RenderItemOpenDoc(std::string fileName)
      : RenderItem(Type::OpenDoc), docFile(std::move(fileName)) {}
  ~RenderItemOpenDoc() override = default;
};

struct RenderItemRun : RenderItem {
  std::function<void()> fun;
  explicit RenderItemRun(std::invocable auto fun)
      : RenderItem(Type::Run), fun(std::move(fun)) {}
  ~RenderItemRun() override = default;
  void operator()() const { fun(); }
};

// Abstract interface for the renderer used by external components
class AbstractRenderer {
protected:
  TQueue<RenderItem> renderQueue;
  AppStateRef state;
  std::thread::id renderThreadId;

  // token to keep anything other than Renderer::create from using our
  // constructor
  struct Private {
    explicit Private() = default;
  };

public:
  /**
   * @brief Construct a Renderer.
   * Prefer using concrete subclass' ::create() method to enforce correct
   * ownership semantics.
   * @param state Application state reference.
   * @param _priv Private tag to restrict construction.
   */
  AbstractRenderer(AppStateRef state, [[maybe_unused]] Private _priv)
      : state(std::move(state)) {}
  virtual ~AbstractRenderer() = default;

  // Entry point for the render thread function
  virtual void operator()(AutoSDLWindow &window) = 0;

  /**
   * @brief Schedule arbitrary work to run on the render thread.
   * If called from the render thread, executes immediately; otherwise enqueues.
   * @param fun Callable with no arguments.
   */
  void run(std::invocable auto fun) {
    if (std::this_thread::get_id() == renderThreadId) {
      fun();
    } else {
      renderQueue.push(RenderItemRun(fun));
    }
  }
  /**
   * @brief Schedule work that needs a AbstractRenderer* on the render thread.
   * If called from the render thread, executes immediately; otherwise enqueues.
   * @param fun Callable taking Renderer*.
   */
  void run(std::invocable<AbstractRenderer *> auto fun) {
    if (std::this_thread::get_id() == renderThreadId) {
      fun(this);
    } else {
      renderQueue.push(RenderItemRun(std::bind(fun, this)));
    }
  }

  template <typename Item>
    requires std::derived_from<Item, RenderItem>
  void push(const Item &item) {
    renderQueue.push(item);
  }

  /**
   * @brief Get the default font name from application state.
   */
  [[nodiscard]] std::string_view defaultFontName() const {
    return state->defaultFontName;
  }
};

using RendererRef = std::shared_ptr<AbstractRenderer>;

/**
 * @class Renderer
 * @brief Drives the render loop against a backend-neutral RenderDevice.
 *
 * There is one renderer for every graphics API: the device abstraction hides
 * the differences, so the loop, the queue handling and the document management
 * are shared rather than duplicated per backend.
 */
class Renderer : public AbstractRenderer,
                 public Loggable,
                 public std::enable_shared_from_this<Renderer> {
private:
  /// Which API this renderer's device drives.
  render::Backend backendKind;
  /// Device owned for the lifetime of the render loop.
  std::unique_ptr<render::RenderDevice> device;
  /// Notifications drawn over the documents. Built once the device is up, and
  /// destroyed before it goes down: it owns device storage.
  std::unique_ptr<ToastOverlay> toasts;
  /// The editing caret, likewise built and destroyed around the device.
  std::unique_ptr<Caret> caret;
  /// Pixel of a click whose picking result has not come back yet. Picking is
  /// asynchronous, so the click cannot be answered in the frame that saw it.
  std::optional<std::pair<int, int>> awaitingClick;
  /// Whether that pending answer extends the selection rather than replacing
  /// the caret.
  bool awaitingDrag{};
  /// Scratch for the highlight table, kept so a drag does not reallocate it
  /// every frame.
  std::vector<render::HighlightRange> highlights;
  /// Background the selection paints behind glyphs.
  static constexpr std::uint32_t selectionColour = 0x99C4FFFFU;
  /// In-flight background document loads. These capture the render thread's
  /// RenderState by reference, so the render loop waits on them before
  /// returning.
  std::vector<std::future<void>> pendingDocLoads;

  /// Most recent completed picking result, kept so that interactive callers can
  /// consult it without polling the device themselves.
  std::optional<render::PickingResult> lastPick;
  /// Last tag reported to the log, so that hovering over one object does not
  /// print a line per frame.
  render::PickingTag reportedPick{};
  /// Index of the next AppState::requestedPicks entry to issue.
  std::size_t nextPick{};
  /// How many of those queries have come back.
  std::size_t picksReported{};
  /// Index of the next AppState::requestedClicks entry to issue, and how many
  /// have been answered.
  std::size_t nextClick{};
  std::size_t clicksReported{};
  bool selectionApplied{};
  /// Drop loads that have already finished, so the list cannot grow without
  /// bound over the lifetime of the process.
  void reapFinishedDocLoads();
  /// True while the render queue is non-empty or a document load is still
  /// running.
  [[nodiscard]] bool hasPendingWork() const;
  /// Run one queued command.
  void dispatch(RenderState &state, RenderItem &item);
  /// Drain picking reads that have completed since the last frame.
  void collectPickingResults(RenderState &state);
  /// Apply --select, once any requested clicks have been answered.
  void applySelectionRequest(bool settled);
  /// Rebuild and upload the highlight spans covering the selection.
  void updateHighlights(RenderState &state);
  /// Insert anything typed since the last frame at the caret.
  void applyTypedText(RenderState &state);
  /// Place the caret from a picking result that answered a click.
  void placeCaretFromPick(RenderState &state, const render::PickingResult &pick);
  /// Turn driver diagnostics recorded since the last frame into notifications.
  /// The device has already logged them; this is what puts them on screen.
  void collectDiagnostics(RenderState &state);

protected:
  /**
   * Open an existing document file and prepare it for rendering.
   */
  void openDoc(RenderState &state, std::string &fileName);

  /**
   * Create a new empty document and initialize any default resources.
   */
  void newDoc(RenderState &state);

  /**
   * Draw one frame.
   * @param settled True when nothing is queued and every document has finished
   *        loading, so the frame shows the finished result.
   * @return true if the render loop should continue, false to exit.
   */
  bool update(RenderState &state, bool settled);

  /// Build the glyph pipeline from the portable shader sources, and the
  /// overlay pipeline that shares them.
  void createPipeline(RenderState &state) const;

  /// The loop proper. Separated from operator() so that everything it does,
  /// including device creation, is covered by one exception handler.
  void renderLoop(AutoSDLWindow &window);

public:
  /**
   * @brief Factory method; constructs a Renderer for @p backend.
   */
  static RendererRef create(const AppStateRef &appState,
                            render::Backend backend) {
    return std::make_shared<Renderer>(appState, backend, Private());
  }

  /**
   * @brief Convenience to get a shared_ptr to this instance.
   */
  std::shared_ptr<Renderer> getPtr() { return shared_from_this(); }

  Renderer(const AppStateRef &state, render::Backend backend,
           [[maybe_unused]] Private _priv)
      : AbstractRenderer(state, _priv), backendKind(backend) {}
  ~Renderer() override;

  /**
   * @brief Main render loop entry point; runs until the application requests
   * exit.
   */
  void operator()(AutoSDLWindow &window) override;
};

#endif // GLEDITOR_RENDERER_H

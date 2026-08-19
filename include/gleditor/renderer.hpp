#ifndef GLEDITOR_RENDERER_H
#define GLEDITOR_RENDERER_H

#include <choreograph/Choreograph.h>
#include <chrono>
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
#include <gleditor/a11y/documents.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/draw_budget.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/span_decorator.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text_source.hpp>
#include <gleditor/toast.hpp>

class Doc;
struct AutoSDLWindow;
struct RenderState;

struct RenderItem {
  enum class Type : std::uint8_t {
    NewDoc,
    CloseDoc,
    Resize,
    OpenDoc,
    SaveDoc,
    Run,
    RunState,
  };
  Type type;

  explicit RenderItem(const Type type) : type(type) {}
  virtual ~RenderItem()                     = default;
  RenderItem(const RenderItem &)            = default;
  RenderItem &operator=(const RenderItem &) = default;
  RenderItem(RenderItem &&)                 = default;
  RenderItem &operator=(RenderItem &&)      = default;
};
struct RenderItemNewDoc : RenderItem {
  RenderItemNewDoc() : RenderItem(Type::NewDoc) {}
};

/**
 * @brief Close a document, fading it out first.
 *
 * The index is the document's position among the open ones. The default names
 * the most recently opened, which is what a keystroke with no other way to say
 * which document it meant should do.
 */
struct RenderItemCloseDoc : RenderItem {
  static constexpr std::uint32_t mostRecent = ~0U;
  std::uint32_t docIndex;
  explicit RenderItemCloseDoc(const std::uint32_t index = mostRecent)
      : RenderItem(Type::CloseDoc), docIndex(index) {}
};

/**
 * @brief Write a document's current text back to wherever it was opened from.
 *
 * The index is the document's position among the open ones, defaulting to the
 * most recently opened, the same as RenderItemCloseDoc and for the same
 * reason. A document that has never been named -- created with "new" rather
 * than opened from somewhere -- has nowhere to write to yet; see
 * Renderer::saveDoc for what that does instead of guessing a name.
 */
struct RenderItemSaveDoc : RenderItem {
  static constexpr std::uint32_t mostRecent = ~0U;
  std::uint32_t docIndex;
  explicit RenderItemSaveDoc(const std::uint32_t index = mostRecent)
      : RenderItem(Type::SaveDoc), docIndex(index) {}
};

struct RenderItemResize : RenderItem {
  int width, height;
  RenderItemResize(const int width, const int height)
      : RenderItem(Type::Resize), width(width), height(height) {}
};

/**
 * @brief Open a document over whatever text @p source supplies.
 *
 * The source is shared rather than copied because the queue is what carries it
 * across to the render thread, and it is consulted from a loader thread after
 * that: the caller cannot know when it has been finished with.
 */
struct RenderItemOpenDoc : RenderItem {
  std::shared_ptr<gleditor::TextSource> source;
  explicit RenderItemOpenDoc(std::shared_ptr<gleditor::TextSource> aSource)
      : RenderItem(Type::OpenDoc), source(std::move(aSource)) {}
  /// Convenience for the common case: a document that is a file on disk.
  explicit RenderItemOpenDoc(const std::string &fileName)
      : RenderItem(Type::OpenDoc),
        source(std::make_shared<gleditor::FileTextSource>(fileName)) {}
};

struct RenderItemRun : RenderItem {
  std::function<void()> fun;
  explicit RenderItemRun(std::invocable auto fun)
      : RenderItem(Type::Run), fun(std::move(fun)) {}
  void operator()() const { fun(); }
};

/**
 * @brief Work that needs the render thread's own state.
 *
 * Editing a document takes a RenderState, because an edit schedules a reflow
 * against the glyph cache and the buffer pool. That state is created inside
 * the render loop and exists nowhere else, so a program with a key bound to
 * "delete the selection" had no way to reach it. This is that way.
 */
struct RenderItemRunState : RenderItem {
  std::function<void(RenderState &)> fun;
  explicit RenderItemRunState(std::invocable<RenderState &> auto fun)
      : RenderItem(Type::RunState), fun(std::move(fun)) {}
  void operator()(RenderState &state) const { fun(state); }
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

  /**
   * @brief Schedule work that needs the render thread's RenderState.
   *
   * Always enqueued, even when called from the render thread: the state
   * belongs to the render loop's own stack frame, so there is no way to hand
   * it over other than by going round the queue.
   */
  void runWithState(std::invocable<RenderState &> auto fun) {
    renderQueue.push(RenderItemRunState(std::move(fun)));
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

  /**
   * @brief Access the shared application state.
   */
  [[nodiscard]] AppStateRef appState() const { return state; }

  /**
   * @brief Register something that colours ranges of a document, or draws into
   *        the frame.
   *
   * Neither is owned, and both are consulted from the render thread every
   * frame: register before the render thread starts, or from inside run().
   * Registering the same one twice registers it once.
   */
  /**
   * @brief The editing caret, or nullptr before the render thread has built
   *        one.
   *
   * A program with a command that acts on the selection -- delete it, quote
   * it, link it -- has to be able to ask what is selected. Only safe to touch
   * on the render thread, so reach it from inside run() or runWithState().
   */
  [[nodiscard]] virtual Caret *editCaret() { return nullptr; }

  /**
   * @brief World units between the resting places of adjacent documents.
   *
   * Public because a program that moves a document -- to bring one next to
   * another it is connected to, say -- has to be able to say "one place over"
   * in the same units the renderer arranges them in. Guessing a distance would
   * leave the document somewhere no other document ever sits.
   */
  static constexpr float docSpacing = 50.0F;

  /// Where the document at @p index rests when nothing is animating. The one
  /// place the row is described, so opening, closing, moving and anything a
  /// program does to a document cannot disagree about it.
  [[nodiscard]] static glm::vec3 documentSlot(const std::size_t index) {
    return {docSpacing * static_cast<float>(index), 0.0F, 0.0F};
  }

  void addSpanDecorator(gleditor::SpanDecorator *decorator);
  void removeSpanDecorator(gleditor::SpanDecorator *decorator);
  void addFrameContributor(gleditor::FrameContributor *contributor);
  void removeFrameContributor(gleditor::FrameContributor *contributor);
  /// Something that wants first refusal on clicks, so that a program can be
  /// told when one lands on what it drew. Offered in registration order.
  void addPickObserver(gleditor::PickObserver *observer);
  void removePickObserver(gleditor::PickObserver *observer);

protected:
  std::vector<gleditor::SpanDecorator *> spanDecorators;
  std::vector<gleditor::FrameContributor *> frameContributors;
  std::vector<gleditor::PickObserver *> pickObservers;
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
  /// What the open documents look like to an assistive technology. Built from
  /// the same state as the frame, once a frame, and only when it has changed.
  gleditor::a11y::DocumentsSource documents;
  /// Pixel of a click whose picking result has not come back yet. Picking is
  /// asynchronous, so the click cannot be answered in the frame that saw it.
  std::optional<std::pair<int, int>> awaitingClick;
  /// Whether that pending answer extends the selection rather than replacing
  /// the caret.
  bool awaitingDrag{};
  /// Scratch for the highlight table, kept so a drag does not reallocate it
  /// every frame.
  std::vector<render::HighlightRange> highlights;
  /// Scratch the registered decorators are asked to fill, likewise reused.
  std::vector<gleditor::SpanStyle> decoratedSpans;
  /// Background the selection paints behind glyphs.
  static constexpr std::uint32_t selectionColour = 0x99C4FFFFU;
  /// In-flight background document loads. These capture the render thread's
  /// RenderState by reference, so the render loop waits on them before
  /// returning.
  std::vector<std::future<void>> pendingDocLoads;

  /**
   * @brief Every animation in flight, stepped once per frame.
   *
   * Choreograph does no locking, so this is touched from the render thread
   * only -- which is also the thread that opens documents and posts toasts,
   * the two things that start a motion.
   */
  ch::Timeline timeline;
  /// When the previous frame stepped the timeline, so the step is in real time
  /// rather than in frames: the same fade has to last as long on a software
  /// rasteriser as on a GPU.
  std::optional<std::chrono::steady_clock::time_point> lastAnimationStep;
  /// Advance every animation to now. Returns the seconds that passed.
  double stepAnimations();
  /**
   * @brief Documents that have been closed but are still fading out.
   *
   * Held apart from the open list so that an index into that list, which the
   * picking tags carry, keeps meaning the same document. They are drawn and
   * nothing else.
   */
  std::vector<std::shared_ptr<Doc>> fadingDocs;

  /// Most recent completed picking result, kept so that interactive callers can
  /// consult it without polling the device themselves.
  std::optional<render::PickingResult> lastPick;
  /// Last tag reported to the log, so that hovering over one object does not
  /// print a line per frame.
  render::PickingTag reportedPick{};
  /// Index of the next AppState::script step to carry out.
  std::size_t nextStep{};
  /// Whether the step being carried out is waiting for a picking answer. One
  /// at a time: the device holds only a couple of reads, and a step that
  /// issued its own before the last was answered would lose one of them.
  bool awaitingStep{};
  /// The pixel a scripted --pick is waiting on, told apart from a click by
  /// what happens with the answer: a pick is reported, a click places the
  /// caret.
  std::optional<std::pair<int, int>> awaitingPick;
  /// Whether the step being carried out is waiting for work it scheduled --
  /// the reflow an edit causes -- rather than for a picking answer.
  bool awaitingSettle{};
  /// Wall time of each settled frame, of collecting its page draws, and of
  /// handing them to the device. Gathered only when --benchmark asked for it.
  std::vector<std::chrono::nanoseconds> benchFrame;
  std::vector<std::chrono::nanoseconds> benchCollect;
  std::vector<std::chrono::nanoseconds> benchRecord;
  /// Page draws the last measured frame submitted, reported alongside the
  /// timings: a recording cost means nothing without the number of draws.
  std::size_t benchBatches{};
  /// What the last frame's collection decided: how many pages it considered,
  /// skipped as off screen, and drew coarsely. Reported by --benchmark, since
  /// culling that is not counted is culling nobody can check.
  DrawStats lastDraw{};
  /// Print the gathered timings. Reports the median rather than the mean: a
  /// software rasteriser under a virtual display produces occasional
  /// hundred-millisecond frames that no amount of averaging removes.
  void reportBenchmark() const;
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
  /// Carry out the next automation step once the one before it has finished.
  void advanceScript(RenderState &state);
  /// Finish the step just carried out, or wait for the work it scheduled.
  void finishStepWhenSettled();
  /// True when every automation step has been carried out and answered, which
  /// is when a screenshot shows what the script asked for rather than the
  /// middle of it.
  [[nodiscard]] bool scriptFinished() const {
    return nextStep >= state->script.size() && !awaitingStep;
  }
  /// Rebuild and upload the highlight spans covering the selection.
  void updateHighlights(RenderState &state);
  /// Insert anything typed since the last frame at the caret.
  void applyTypedText(RenderState &state);
  /// Place the caret from a picking result that answered a click.
  void placeCaretFromPick(RenderState &state,
                          const render::PickingResult &pick);
  /// Turn driver diagnostics recorded since the last frame into notifications.
  /// The device has already logged them; this is what puts them on screen.
  void collectDiagnostics(RenderState &state);

protected:
  /**
   * Open a document over @p source and prepare it for rendering.
   */
  void openDoc(RenderState &state, const gleditor::TextSource &source);

  /**
   * Create a new empty document and initialize any default resources.
   */
  void newDoc(RenderState &state);

  /**
   * @brief Start closing the document at @p index.
   *
   * It leaves the open list at once, so nothing can pick it or type into it,
   * but is kept alive and drawn until its fade finishes. The documents after
   * it are renumbered and eased into the places that opened up.
   */
  void closeDoc(RenderState &state, std::uint32_t index);

  /**
   * @brief Write the document at @p index back to wherever it was opened
   *        from.
   *
   * A toast reports the outcome either way, since a save that failed
   * silently is worse than one that never ran: the document at @p index has
   * no name yet (created with "new"), the write itself failed (permissions,
   * a full disk, a revoked Android Uri grant), or it succeeded.
   */
  void saveDoc(RenderState &state, std::uint32_t index);

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

  [[nodiscard]] Caret *editCaret() override { return caret.get(); }

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

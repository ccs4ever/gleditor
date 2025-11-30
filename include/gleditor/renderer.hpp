#ifndef GLEDITOR_RENDERER_H
#define GLEDITOR_RENDERER_H

#include <gleditor/tqueue.hpp>
#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gleditor/gl/gl.hpp>
#include <gleditor/gl/state.hpp>
#include <gleditor/log.hpp>
#include <gleditor/state.hpp>

struct AutoSDLWindow;

struct RenderItem {
  enum class Type : std::uint8_t {
    NewDoc,
    Resize,
    OpenDoc,
    Run,
  };
  Type type;

  explicit RenderItem(Type type) : type(type) {}
  virtual ~RenderItem() = default;
};
struct RenderItemNewDoc : public RenderItem {
  RenderItemNewDoc() : RenderItem(RenderItem::Type::NewDoc) {}
  ~RenderItemNewDoc() override = default;
};

struct RenderItemResize : public RenderItem {
  int width, height;
  RenderItemResize(int width, int height)
      : RenderItem(RenderItem::Type::Resize), width(width), height(height) {}
  ~RenderItemResize() override = default;
};

struct RenderItemOpenDoc : public RenderItem {
  std::string docFile;
  explicit RenderItemOpenDoc(std::string  fileName)
      : RenderItem(RenderItem::Type::OpenDoc), docFile(std::move(fileName)) {}
  ~RenderItemOpenDoc() override = default;
};

struct RenderItemRun : public RenderItem {
  std::function<void()> fun;
  explicit RenderItemRun(std::invocable auto fun)
      : RenderItem(RenderItem::Type::Run), fun(fun) {}
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
   * Prefer using concrete subclass' ::create() method to enforce correct ownership semantics.
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
    renderQueue.push(std::move(item));
  }

  /**
   * @brief Get the default font name from application state.
   */
  std::string_view defaultFontName() const { return state->defaultFontName; }

};

using RendererRef = std::shared_ptr<AbstractRenderer>;

/**
 * @class Renderer
 * @brief Manages the rendering loop, OpenGL state, and render-queue processing.
 *
 * Renderer owns an internal render thread context, a queue of RenderItem
 * commands, and utilities for creating and binding an off-screen framebuffer
 * used for color/picking passes. Use Renderer::create() to construct an
 * instance and call operator()(AutoSDLWindow&) to run the render loop.
 */
class Renderer : public AbstractRenderer,
                 public Loggable,
                 public std::enable_shared_from_this<Renderer> {
private:
  unsigned int pickingFBO{}, pickingRBO{}, colorRBO{}, depthRBO{};

protected:
  /**
   * Open an existing document file and prepare it for rendering.
   *
   * @param glState Current OpenGL state wrapper.
   * @param window SDL window wrapper associated with the render context.
   * @param fileName Path to the document file to open (modified as needed).
   */
  void openDoc(GLState &glState, AutoSDLWindow &window, std::string &fileName);

  /**
   * Create a new empty document and initialize any default resources.
   *
   * @param glState Current OpenGL state wrapper.
   * @param window SDL window wrapper associated with the render context.
   */
  void newDoc(GLState &glState, AutoSDLWindow &window);

  /**
   * Perform GL setup that depends on window/context size or state.
   *
   * Creates off-screen framebuffers, configures attachments, and prepares
   * state for subsequent rendering.
   *
   * @param glState Read-only OpenGL state wrapper.
   */
  void setupGL(const GLState &glState);

  /**
   * Handle window or framebuffer resize events.
   *
   * Recreates dependent GL resources (e.g., renderbuffers) and updates
   * viewport-related values.
   */
  void resize();

  /**
   * Update application and rendering state for one frame.
   *
   * Processes the render queue, handles input-driven actions, and may request
   * window swaps.
   *
   * @param glState Current OpenGL state wrapper.
   * @param window SDL window wrapper associated with the render context.
   * @return true if the render loop should continue, false to exit.
   */
  bool update(const GLState &glState, const AutoSDLWindow &window);

  /**
   * Initialize OpenGL extensions, debug output, and static GL resources.
   */
  void initGL();

public:
  /**
   * @brief RAII helper that binds a framebuffer on construction and restores
   *        the previous binding on destruction.
   *
   * Use this to temporarily bind the Renderer-managed picking FBO to a given
   * target (e.g., GL_DRAW_FRAMEBUFFER) within a scope.
   */
  struct AutoFBO {
    const Renderer *renderer;  ///< Owning renderer whose FBO will be bound.
    GLenum target;             ///< Framebuffer binding target.
    /**
     * Construct and bind the renderer's FBO to the specified target.
     * @param aRenderer Renderer that owns the FBO.
     * @param aTarget GL framebuffer target to bind (e.g., GL_DRAW_FRAMEBUFFER).
     */
    AutoFBO(const Renderer *aRenderer, GLenum aTarget)
        : renderer(aRenderer), target(aTarget) {
      renderer->bindFBO(target);
    }
    /**
     * Destructor unbinds the FBO from the target (binds 0).
     */
    ~AutoFBO() { Renderer::clearFBO(target); }
  };
  /**
   * @brief Bind the internal picking FBO to the given target and set draw buffers.
   * @param target GL framebuffer target (e.g., GL_DRAW_FRAMEBUFFER).
   */
  void bindFBO(GLenum target) const {
    glBindFramebuffer(target, pickingFBO);
    std::array<unsigned int, 2> arr = {GL_COLOR_ATTACHMENT0,
                                       GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, arr.data());
  }
  /**
   * @brief Unbind any FBO from the specified target (binds default framebuffer).
   * @param target GL framebuffer target (e.g., GL_DRAW_FRAMEBUFFER).
   */
  static void clearFBO(GLenum target) { glBindFramebuffer(target, 0); }

  /**
   * @brief Factory method; constructs a Renderer instance.
   * @param appState Shared application state used by the renderer.
   * @return Shared pointer to a new Renderer.
   */
  static RendererRef create(const AppStateRef& appState) {
    return std::make_shared<Renderer>(appState, Private());
  }

  /**
   * @brief Convenience to get a shared_ptr to this instance.
   * Requires that this object was created via shared_ptr (see create()).
   */
  std::shared_ptr<Renderer> getPtr() { return shared_from_this(); }

  /**
   * @brief Construct a Renderer.
   * Prefer using Renderer::create() to enforce correct ownership semantics.
   * @param state Application state reference.
   * @param _priv Private tag to restrict construction.
   */
  Renderer(const AppStateRef &state, [[maybe_unused]] Private _priv)
      : AbstractRenderer(state, _priv) {}

  /**
   * @brief Main render loop entry point; runs until the application requests exit.
   * @param window SDL window wrapper associated with the GL context.
   */
  void operator()(AutoSDLWindow &window) override;



};


#endif // GLEDITOR_RENDERER_H

/**
 * @file renderer.cpp
 * @brief The render loop, shared by every graphics backend.
 */
#include <gleditor/renderer.hpp> // IWYU pragma: associated

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/string_cast.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_wrap.hpp>
#include <gleditor/state.hpp>
#include <gleditor/tqueue.hpp>

namespace {

/// Directory the portable shader bodies are read from, relative to the working
/// directory the application is started in.
constexpr const char *shaderDir = "assets/shaders";

/// Copy a matrix into the flat array the device uniform structs carry.
std::array<float, 16> toArray(const glm::mat4 &mat) {
  std::array<float, 16> out{};
  const auto *src = glm::value_ptr(mat);
  std::copy_n(src, out.size(), out.begin());
  return out;
}

/**
 * @brief Write a captured frame as a binary PPM.
 *
 * PPM is chosen because it needs no image library, which keeps the comparison
 * between backends free of any encoding differences of its own.
 */
void writeScreenshot(const render::FrameImage &image, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    std::cerr << "failed to open screenshot path: " << path << "\n";
    return;
  }
  out << std::format("P6\n{} {}\n255\n", image.width, image.height);
  for (std::size_t i = 0; i + 3 < image.rgba.size() + 1; i += 4) {
    const std::array<char, 3> rgb = {static_cast<char>(image.rgba[i]),
                                     static_cast<char>(image.rgba[i + 1]),
                                     static_cast<char>(image.rgba[i + 2])};
    out.write(rgb.data(), rgb.size());
  }
  std::cout << std::format("wrote screenshot {} ({}x{})\n", path, image.width,
                           image.height);
}

} // namespace

Renderer::~Renderer() = default;

void Renderer::createPipeline(RenderState &state) const {
  render::PipelineDesc desc;
  desc.name = "glyph";
  desc.vertexSource =
      render::readShaderBody(std::string(shaderDir) + "/glyph.vert.glsl");
  desc.fragmentSource =
      render::readShaderBody(std::string(shaderDir) + "/glyph.frag.glsl");
  desc.spirvDir = std::string(shaderDir) + "/vulkan";
  desc.layout   = Doc::vertexLayout();

  state.glyphPipeline = device->createPipeline(desc);
}

void Renderer::newDoc(RenderState &state) {
  const auto docPtr = Doc::create(getPtr(), device.get(), glm::mat4(1.0));
  state.docs.push_back(docPtr->getPtr());
}

void Renderer::reapFinishedDocLoads() {
  const auto done = std::ranges::remove_if(pendingDocLoads, [](auto &fut) {
    return !fut.valid() ||
           std::future_status::ready == fut.wait_for(std::chrono::seconds{0});
  });
  pendingDocLoads.erase(done.begin(), done.end());
}

bool Renderer::hasPendingWork() const {
  return !renderQueue.empty() || !pendingDocLoads.empty();
}

void Renderer::openDoc(RenderState &state, std::string &fileName) {
  const auto numDocsOpened = static_cast<double>(state.docs.size());
  const auto newDocPosition =
      glm::translate(glm::mat4(1.0), glm::vec3(50.0 * numDocsOpened, 0.0, 0.0));
  std::cout << "doc pos: " << numDocsOpened << " "
            << glm::to_string(newDocPosition) << "\n";
  auto docPtr = Doc::create(getPtr(), device.get(), newDocPosition, fileName);
  reapFinishedDocLoads();
  pendingDocLoads.push_back(std::async(
      std::launch::async, [&state, docPtr] { docPtr->makePages(state); }));
  state.docs.push_back(docPtr->getPtr());
}

bool Renderer::update(RenderState &state, const bool settled) {
  // avoid expensive rendering if we are dead
  if (!state.device || !this->state->alive) {
    return false;
  }

  const auto start = std::chrono::steady_clock::now();

  if (!device->beginFrame()) {
    return this->state->alive;
  }

  device->bindPipeline(state.glyphPipeline);

  {
    std::lock_guard locker(this->state->view);
    const auto &view = this->state->view;

    const glm::mat4 projection = glm::perspective(
        glm::radians(view.fov),
        static_cast<float>(view.screenWidth) /
            static_cast<float>(view.screenHeight),
        0.1F, 10000.0F);
    const glm::mat4 camera =
        glm::lookAt(view.pos, view.pos + view.front, view.upward);

    device->setFrameUniforms(
        render::FrameUniforms{toArray(projection), toArray(camera)});
  }

  device->bindGlyphTexture(state.glyphCache.textureHandle());

  for (const std::shared_ptr<Doc> &doc : state.docs) {
    doc->draw(state);
  }

  device->endFrame();

  // Capture after the frame is complete: the colour target still holds its
  // contents, and reading a finished frame avoids interrupting one that the
  // device has already begun submitting.
  if (settled && !this->state->screenshotPath.empty()) {
    writeScreenshot(device->captureColorTarget(), this->state->screenshotPath);
    this->state->screenshotPath.clear();
  }

  const auto end              = std::chrono::steady_clock::now();
  this->state->frameTimeDelta = end - start;

  return this->state->alive;
}

void Renderer::dispatch(RenderState &state, RenderItem &item) {
  switch (item.type) {
  case RenderItem::Type::NewDoc: {
    newDoc(state);
    break;
  }
  case RenderItem::Type::Resize: {
    const auto &resize = dynamic_cast<RenderItemResize &>(item);
    device->resize(resize.width, resize.height);
    break;
  }
  case RenderItem::Type::OpenDoc: {
    openDoc(state, dynamic_cast<RenderItemOpenDoc &>(item).docFile);
    break;
  }
  case RenderItem::Type::Run: {
    dynamic_cast<const RenderItemRun &>(item)();
    break;
  }
  }
}

void Renderer::operator()(AutoSDLWindow &window) {

  this->renderThreadId = std::this_thread::get_id();

  try {
    renderLoop(window);
  } catch (const std::exception &err) {
    // Nothing above this frame can catch: it is the top of the render thread,
    // and letting the exception escape would call std::terminate instead of
    // reporting what went wrong. Clearing `alive` also releases the main thread
    // from its event loop.
    std::cerr << std::format("render thread ({} backend) failed: {}\n",
                             render::backendName(backendKind), err.what());
    this->state->alive = false;
  }
}

void Renderer::renderLoop(AutoSDLWindow &window) {
  device = render::createDevice(backendKind);
  device->initialize(window);

  RenderState state(device.get());

  createPipeline(state);

  while (this->state->alive) {

    // Drain queued commands before drawing, so that work requested before this
    // thread started -- files named on the command line, for instance -- is
    // carried out rather than discarded on the first frame.
    while (auto item = renderQueue.pop()) {
      dispatch(state, *item);
    }

    reapFinishedDocLoads();

    // Everything queued has been carried out and every document has finished
    // loading, so this frame shows the finished result. That is the frame a
    // screenshot should capture, and the point at which --profile may quit.
    const bool settled = !hasPendingWork();

    // still want to update once even if we don't have anything in the render
    // queue
    if (!update(state, settled)) {
      break;
    }

    if (settled && this->state->profiling) {
      this->state->alive = false;
      break;
    }
  }

  // The background loaders capture the render state, which lives on this stack
  // frame, so none of them may outlive this function.
  for (auto &fut : pendingDocLoads) {
    if (fut.valid()) {
      fut.wait();
    }
  }
  pendingDocLoads.clear();

  // Documents own device buffers; they must be released while the device is
  // still alive, and after any in-flight frame has finished reading them.
  device->waitIdle();
  state.docs.clear();
  device->shutdown();
}
// vi: set sw=2 sts=2 ts=2 et:

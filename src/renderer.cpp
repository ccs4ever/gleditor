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
  // The overlay draws the same glyph instances with the same shaders; only the
  // depth state and the transform it is handed differ.
  toasts->createPipeline(desc);
  caret->createPipeline(desc);
}

void Renderer::newDoc(RenderState &state) {
  const auto docPtr = Doc::create(getPtr(), device.get(), glm::mat4(1.0));
  docPtr->setDocIndex(static_cast<std::uint32_t>(state.docs.size()));
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
  docPtr->setDocIndex(static_cast<std::uint32_t>(state.docs.size()));
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

  // Collect picking reads issued on earlier frames before starting this one.
  // Doing it here rather than after the draws means a request is never
  // answered within the frame that made it, so the asynchronous path is the
  // path that always runs -- on a driver quick enough to finish the read
  // immediately as much as on one that is not.
  collectPickingResults(state);
  collectDiagnostics(state);
  applySelectionRequest(settled);
  applyTypedText(state);

  if (!device->beginFrame()) {
    return this->state->alive;
  }

  glm::mat4 viewProjection(1.0F);
  int screenWidth  = 0;
  int screenHeight = 0;
  {
    std::lock_guard locker(this->state->view);
    const auto &view = this->state->view;
    screenWidth      = view.screenWidth;
    screenHeight     = view.screenHeight;

    const glm::mat4 projection = glm::perspective(
        glm::radians(view.fov),
        static_cast<float>(view.screenWidth) /
            static_cast<float>(view.screenHeight),
        0.1F, 10000.0F);
    const glm::mat4 camera =
        glm::lookAt(view.pos, view.pos + view.front, view.upward);

    viewProjection = projection * camera;
  }

  updateHighlights(state);

  device->bindPipeline(state.glyphPipeline);
  device->bindGlyphTexture(state.glyphCache.textureHandle());

  for (const std::shared_ptr<Doc> &doc : state.docs) {
    doc->draw(state, viewProjection);
  }
  for (const std::shared_ptr<Doc> &doc : state.docs) {
    doc->drawCaret(state, viewProjection, *caret);
  }

  // Last, so that the overlay is on top: its pipeline does not depth test, so
  // submission order is what decides.
  toasts->expire(ToastOverlay::Clock::now());
  toasts->draw(state, screenWidth, screenHeight);

  // Ask what is under the cursor, or at the pixel --pick named. The read is
  // asynchronous, so this only queues it; the answer is collected below on a
  // later frame.
  //
  // Only settled frames are queried: a frame drawn while pages are still being
  // built would answer for a document that is not there yet, which for --pick
  // means reporting an empty tag and exiting.
  if (settled) {
    if (nextPick < this->state->requestedPicks.size()) {
      // One request per frame. The device can only hold a couple of reads at
      // once, and issuing more than it accepts would silently drop queries.
      const auto &[pickX, pickY] = this->state->requestedPicks[nextPick];
      device->requestPickingTag(pickX, pickY);
      nextPick++;
    } else if (nextClick < this->state->requestedClicks.size() &&
               !awaitingClick) {
      // One at a time: a click is only answered when its own read comes back,
      // so issuing the next before then would lose the first.
      const auto &[clickX, clickY] = this->state->requestedClicks[nextClick];
      awaitingClick                = std::pair{clickX, clickY};
      device->requestPickingTag(clickX, clickY);
      nextClick++;
    } else if (this->state->dragPending.exchange(false)) {
      // A drag reuses the click machinery; only what happens with the answer
      // differs, so the pending pixel is remembered as a drag.
      const auto dragX = this->state->dragX.load();
      const auto dragY = this->state->dragY.load();
      awaitingClick    = std::pair{dragX, dragY};
      awaitingDrag     = true;
      device->requestPickingTag(dragX, dragY);
    } else if (this->state->clickPending.exchange(false)) {
      // A click takes priority over the hover query: only one read is issued
      // per frame, and the click is the one somebody is waiting on.
      const auto clickX = this->state->clickX.load();
      const auto clickY = this->state->clickY.load();
      awaitingClick     = std::pair{clickX, clickY};
      awaitingDrag      = false;
      device->requestPickingTag(clickX, clickY);
    } else if (this->state->requestedPicks.empty()) {
      device->requestPickingTag(this->state->mouseX, this->state->mouseY);
    }
  }

  device->endFrame();

  // Capture after the frame is complete: the colour target still holds its
  // contents, and reading a finished frame avoids interrupting one that the
  // device has already begun submitting.
  // Wait for any requested clicks to have been answered: picking is
  // asynchronous, so a frame captured the moment the document settles is one
  // or two frames before the caret those clicks place exists.
  if (settled && clicksReported >= this->state->requestedClicks.size() &&
      !this->state->screenshotPath.empty()) {
    writeScreenshot(device->captureColorTarget(), this->state->screenshotPath);
    this->state->screenshotPath.clear();
  }

  const auto end              = std::chrono::steady_clock::now();
  this->state->frameTimeDelta = end - start;

  return this->state->alive;
}

void Renderer::placeCaretFromPick(RenderState &state,
                                  const render::PickingResult &pick) {
  // Every outcome is reported, including the ones that place no caret. The
  // read is asynchronous, so a line that named only the offset could not be
  // lined up with the click that caused it -- and a silent outcome would drop
  // out of a sequence of clicks entirely, which is exactly the misreading this
  // is here to prevent.
  if (pick.tag.empty()) {
    // Nothing was drawn at that pixel: the click landed outside every
    // document. That puts the editor back to navigating, dropping the caret
    // and any selection with it.
    caret->clear();
    std::cout << std::format("caret {},{}: none\n", pick.x, pick.y);
    return;
  }
  if (render::tagKindOverlay == pick.tag.kind) {
    // The caret and the notifications are drawn over the text and write the
    // picking attachment too, so a click can land on one. Leaving the caret
    // alone is the sensible reading of clicking the caret; clearing it would
    // mean the caret could not be clicked on at all.
    std::cout << std::format("caret {},{}: overlay, unchanged\n", pick.x,
                             pick.y);
    return;
  }
  if (pick.tag.docIndex >= state.docs.size()) {
    std::cout << std::format("caret {},{}: no such document {}\n", pick.x,
                             pick.y, pick.tag.docIndex);
    return;
  }
  const auto &doc = state.docs[pick.tag.docIndex];
  const auto offset = doc->offsetForPick(pick.tag);
  if (!offset) {
    std::cout << std::format("caret {},{}: unresolved\n", pick.x, pick.y);
    return;
  }
  if (awaitingDrag) {
    // Dragging keeps the anchor where the press landed and moves the caret,
    // which is what grows the selection.
    caret->extendTo(*offset);
    std::cout << std::format("select {},{}: doc {} [{},{})\n", pick.x, pick.y,
                             pick.tag.docIndex, caret->selectionStart(),
                             caret->selectionEnd());
    return;
  }
  caret->placeAt(pick.tag.docIndex, *offset);
  std::cout << std::format("caret {},{}: doc {} offset {}\n", pick.x, pick.y,
                           pick.tag.docIndex, *offset);
}

void Renderer::collectPickingResults(RenderState &state) {
  while (const auto pick = device->takePickingTag()) {
    lastPick = pick;
    if (awaitingClick && awaitingClick->first == pick->x &&
        awaitingClick->second == pick->y) {
      awaitingClick.reset();
      placeCaretFromPick(state, *pick);
      clicksReported++;
    }
    if (!this->state->requestedPicks.empty()) {
      std::cout << std::format(
          "pick {},{}: kind {} doc {} page {} cluster {} frac {:.3f}\n", pick->x,
          pick->y, pick->tag.kind, pick->tag.docIndex, pick->tag.pageIndex,
          pick->tag.clusterIndex, pick->tag.fraction);
      picksReported++;
    } else if (!pick->tag.sameObject(reportedPick)) {
      // Report transitions rather than every frame: the query runs each frame,
      // but what a reader cares about is the cursor moving onto something new.
      reportedPick = pick->tag;
      if (pick->tag.empty()) {
        std::cout << std::format("no object at {},{}\n", pick->x, pick->y);
      } else {
        std::cout << std::format(
            "tagged object at {},{}: kind {} doc {} page {} cluster {}\n",
            pick->x, pick->y, pick->tag.kind, pick->tag.docIndex,
            pick->tag.pageIndex, pick->tag.clusterIndex);
      }
    }
  }
}

void Renderer::applySelectionRequest(const bool settled) {
  // Applied only once every requested click has been answered. A click
  // replaces a selection rather than extending one, so applying this at
  // startup meant a later --click silently threw it away -- and the frame that
  // was supposed to show a partly highlighted quad showed none.
  if (selectionApplied || !settled || !this->state->requestedSelection ||
      clicksReported < this->state->requestedClicks.size()) {
    return;
  }
  const auto &[from, to] = *this->state->requestedSelection;
  caret->placeAt(0, from);
  caret->anchorSelection();
  caret->extendTo(to);
  selectionApplied = true;
  std::cout << std::format("select: doc 0 [{},{})\n", caret->selectionStart(),
                           caret->selectionEnd());
}

void Renderer::updateHighlights(RenderState &state) {
  // Rebuilt every frame rather than cached: a drag moves the selection each
  // frame anyway, and the table is a handful of entries -- one per page the
  // selection touches.
  highlights.clear();
  if (caret->hasSelection() && caret->documentIndex() < state.docs.size()) {
    state.docs[caret->documentIndex()]->highlightsFor(
        caret->selectionStart(), caret->selectionEnd(), selectionColour,
        highlights);
  }
  device->setHighlights(highlights);
}

void Renderer::applyTypedText(RenderState &state) {
  if (!caret->active()) {
    return;
  }
  std::string typed;
  {
    const std::lock_guard locker(this->state->typedMutex);
    if (this->state->typedText.empty()) {
      return;
    }
    typed.swap(this->state->typedText);
  }
  if (caret->documentIndex() >= state.docs.size()) {
    return;
  }
  state.docs[caret->documentIndex()]->insert(state, caret->byteOffset(), typed,
                                             caret.get());
}

void Renderer::collectDiagnostics(RenderState &state) {
  // The device logged each of these as the driver reported it; showing them is
  // what this adds. Only messages the user can act on are worth interrupting
  // the view for -- a driver's performance notes are not, and on some drivers
  // there is a steady stream of them.
  for (const auto &[severity, message] : device->takeDiagnostics()) {
    if (render::DiagnosticSeverity::Info == severity) {
      continue;
    }
    toasts->post(severity, message, state);
  }
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
    this->state->renderFailed = true;
    this->state->alive        = false;
  }
}

void Renderer::renderLoop(AutoSDLWindow &window) {
  device = render::createDevice(backendKind);
  device->initialize(window);
  device->setStrictDiagnostics(this->state->strictDiagnostics);

  RenderState state(device.get());
  toasts = std::make_unique<ToastOverlay>(device.get(),
                                          std::string(defaultFontName()));
  caret  = std::make_unique<Caret>(device.get());

  createPipeline(state);

  for (const auto &[severity, message] : this->state->requestedToasts) {
    toasts->post(severity, message, state);
  }

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

    // A pick takes a frame or two to come back, so profiling waits for every
    // requested query rather than exiting with one still outstanding.
    if (settled && this->state->profiling &&
        picksReported >= this->state->requestedPicks.size() &&
        clicksReported >= this->state->requestedClicks.size()) {
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
  caret.reset();
  toasts.reset();
  device->shutdown();
}
// vi: set sw=2 sts=2 ts=2 et:

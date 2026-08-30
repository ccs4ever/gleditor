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

#include <gleditor/android_bootstrap.hpp>
#include <gleditor/animation.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_wrap.hpp>
#include <gleditor/state.hpp>
#include <gleditor/tqueue.hpp>

namespace {

/// Directory the portable shader bodies are read from. Found rather than
/// assumed -- see gleditor::assetDir() -- so that an installed copy works from
/// any working directory.
std::string shaderDir() { return gleditor::assetPath("shaders"); }

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
  desc.name           = "glyph";
  const auto shaders  = shaderDir();
  desc.vertexSource   = render::readShaderBody(shaders + "/glyph.vert.glsl");
  desc.fragmentSource = render::readShaderBody(shaders + "/glyph.frag.glsl");
  desc.spirvDir       = shaders + "/vulkan";
  desc.layout         = Doc::vertexLayout();

  state.glyphPipeline = device->createPipeline(desc);
  // The overlay draws the same glyph instances with the same shaders; only the
  // depth state and the transform it is handed differ.
  toasts->createPipeline(desc);
  caret->createPipeline(desc);
  // Whatever the program draws for itself needs the same two things, and this
  // is the first moment either exists.
  for (auto *const contributor : frameContributors) {
    contributor->deviceReady(*device, desc);
  }
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
  // An animation counts as pending work, which is what keeps a screenshot
  // honest: the frame a capture wants is the finished one, and a document
  // halfway through fading in is not it.
  return !renderQueue.empty() || !pendingDocLoads.empty() ||
         !timeline.empty() ||
         (nullptr != toasts && toasts->fadingIn(ToastOverlay::Clock::now())) ||
         std::ranges::any_of(frameContributors,
                             [](const gleditor::FrameContributor *const one) {
                               return one->busy();
                             });
}

double Renderer::stepAnimations() {
  const auto now = std::chrono::steady_clock::now();
  // The first frame steps by nothing rather than by however long start-up
  // took, which would otherwise finish every animation before it was seen.
  const double elapsed =
      lastAnimationStep
          ? std::chrono::duration<double>(now - *lastAnimationStep).count()
          : 0.0;
  lastAnimationStep = now;
  timeline.step(elapsed);
  return elapsed;
}

void Renderer::closeDoc(RenderState &state, const std::uint32_t index) {
  if (state.docs.empty()) {
    return;
  }
  const auto which = RenderItemCloseDoc::mostRecent == index
                         ? state.docs.size() - 1
                         : static_cast<std::size_t>(index);
  if (which >= state.docs.size()) {
    return;
  }

  // Off the open list first, so that a pick or a keystroke arriving during the
  // fade cannot land on a document that is on its way out.
  auto departing = state.docs[which];
  state.docs.erase(state.docs.begin() + static_cast<std::ptrdiff_t>(which));
  departing->animateDeparture(timeline);
  fadingDocs.push_back(std::move(departing));

  // The survivors close the gap. Renumbering matters as much as the movement:
  // a document's index is what its picking tags resolve through, so leaving it
  // stale would make clicks land on the wrong document.
  for (std::size_t i = 0; i < state.docs.size(); i++) {
    state.docs[i]->setDocIndex(static_cast<std::uint32_t>(i));
    // Z is carried forward from the resting place the document opened at
    // rather than reset to documentSlot(i)'s zero: a document opened into
    // the background (RenderItemOpenDoc::depthZ) stays there when
    // something ahead of it in the list closes, instead of snapping to the
    // foreground plane every other document renumbers onto.
    const auto slot = AbstractRenderer::documentSlot(i);
    state.docs[i]->animateMoveTo(
        timeline, glm::vec3(slot.x, slot.y, state.docs[i]->getModel()[3].z));
  }
}

void Renderer::saveDoc(RenderState &state, const std::uint32_t index) {
  if (state.docs.empty()) {
    return;
  }
  const auto which = RenderItemSaveDoc::mostRecent == index
                         ? state.docs.size() - 1
                         : static_cast<std::size_t>(index);
  if (which >= state.docs.size()) {
    return;
  }

  const auto &doc         = state.docs[which];
  const std::string &name = doc->name();
  // A document made with "new" rather than opened from somewhere has no name
  // to write to, and there is no "Save As" yet (packaging/android/README.md
  // notes the Android side of that same gap for opening one) to ask for one.
  if (name.empty()) {
    toasts->post(render::DiagnosticSeverity::Error,
                 "nothing to save this document as yet", state);
    return;
  }
  const auto &content = doc->contents();

#ifdef __ANDROID__
  // Writes through the content:// Uri this document was opened from, when it
  // was opened that way: name() is that Uri's own internal-storage copy
  // (src/android_bootstrap.cpp's openDocumentFromIntent()), not a path the
  // Uri itself would answer to, so writing name() as a plain file here would
  // silently save over that copy and never reach whatever the user actually
  // shared or opened in. Falls through to the plain write below for
  // anything else -- a file:// Uri, which already named a real path, or a
  // document that was never opened from an intent at all.
  if (gleditor::androidSaveDocument(name, content)) {
    toasts->post(render::DiagnosticSeverity::Info,
                 std::format("saved {}", name), state);
    return;
  }
#endif

  std::ofstream file(name, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    toasts->post(render::DiagnosticSeverity::Error,
                 std::format("save failed: could not open {}", name), state);
    return;
  }
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!file.good()) {
    toasts->post(render::DiagnosticSeverity::Error,
                 std::format("save failed: write error on {}", name), state);
    return;
  }
  toasts->post(render::DiagnosticSeverity::Info, std::format("saved {}", name),
               state);
}

void Renderer::openDoc(RenderState &state, const gleditor::TextSource &source,
                       const float depthZ) {
  // A background document (depthZ < 0) does not take the next seat in the
  // foreground row -- documentSlot(state.docs.size()) counts every open
  // document, background ones included, so it would otherwise land a corpus
  // wedged in X between whatever foreground documents happen to flank it in
  // the open order. Centred at the origin instead, it sits behind the row
  // as a backdrop rather than inside it.
  auto slot = depthZ < 0.0F ? glm::vec3(0.0F, 0.0F, 0.0F)
                            : AbstractRenderer::documentSlot(state.docs.size());
  slot.z    = depthZ;
  const auto newDocPosition = glm::translate(glm::mat4(1.0), slot);
  std::cout << "doc pos: " << state.docs.size() << " "
            << glm::to_string(newDocPosition) << "\n";
  auto docPtr = Doc::create(getPtr(), device.get(), newDocPosition, source);
  docPtr->setDocIndex(static_cast<std::uint32_t>(state.docs.size()));
  // A document opened behind the row settles dimmer than one in it. It is
  // there for context -- a corpus to see the row against -- and at full
  // strength it competes with the row for the same attention; see
  // Doc::setRestingOpacity().
  if (depthZ < 0.0F) {
    docPtr->setRestingOpacity(gleditor::anim::backgroundOpacity);
  }
  docPtr->animateArrival(timeline);
  reapFinishedDocLoads();
  pendingDocLoads.push_back(
      std::async(std::launch::async, [docPtr] { docPtr->makePages(); }));
  state.docs.push_back(docPtr->getPtr());
}

bool Renderer::update(RenderState &state, const bool settled) {
  // avoid expensive rendering if we are dead
  if (!state.device || !this->state->alive) {
    return false;
  }

  const auto start = std::chrono::steady_clock::now();

  // Advance every animation before anything reads a position or an opacity, so
  // that one frame draws one instant rather than a mixture of two.
  stepAnimations();

  // Collect picking reads issued on earlier frames before starting this one.
  // Doing it here rather than after the draws means a request is never
  // answered within the frame that made it, so the asynchronous path is the
  // path that always runs -- on a driver quick enough to finish the read
  // immediately as much as on one that is not.
  collectPickingResults(state);
  collectDiagnostics(state);
  applyTypedText(state);

  if (!device->beginFrame()) {
    return this->state->alive;
  }

  glm::mat4 viewProjection(1.0F);
  int screenWidth  = 0;
  int screenHeight = 0;
  {
    std::scoped_lock locker(this->state->view);
    const auto &view = this->state->view;
    screenWidth      = view.screenWidth;
    screenHeight     = view.screenHeight;

    const auto aspect = (view.screenHeight > 0 && view.screenWidth > 0)
                            ? static_cast<float>(view.screenWidth) /
                                  static_cast<float>(view.screenHeight)
                            : 1.0F;
    const glm::mat4 projection =
        glm::perspective(glm::radians(view.fov), aspect, 0.1F, 10000.0F);
    const glm::mat4 camera =
        glm::lookAt(view.pos, view.pos + view.front, view.upward);

    viewProjection = projection * camera;
  }

  updateHighlights(state);

  // Any glyphs rasterised since the last frame have only reached level zero of
  // the atlas; rebuild the rest of the chain before anything samples it.
  state.glyphCache.flush();

  device->bindPipeline(state.glyphPipeline);
  device->bindGlyphTexture(state.glyphCache.textureHandle());

  // Every page of every open document in one list, then one call. Collecting
  // first is what gives a device the chance to record the run on more than one
  // thread; a device that cannot simply walks it in order.
  const auto collectStart = std::chrono::steady_clock::now();
  state.pageBatches.clear();
  DrawBudget budget;
  budget.screenWidth = static_cast<float>(screenWidth);
  budget.coarseBelow = this->state->coarseBelow;
  budget.cull        = this->state->cullPages;
  lastDraw           = DrawStats{};
  for (const std::shared_ptr<Doc> &doc : state.docs) {
    doc->collect(state.pageBatches, viewProjection, budget, lastDraw);
  }
  // Closed documents still draw while they fade. They are gone from the open
  // list, so this is the only thing that still refers to them, and dropping
  // one the moment it is invisible is what ends that.
  for (const std::shared_ptr<Doc> &doc : fadingDocs) {
    doc->collect(state.pageBatches, viewProjection, budget, lastDraw);
  }
  std::erase_if(fadingDocs, [](const std::shared_ptr<Doc> &doc) {
    return doc->hasFadedOut();
  });
  // Timed apart from the collection above: only the recording can be split
  // across threads, so an improvement there would be invisible in a figure
  // that also counted a matrix multiply per page.
  const auto recordStart = std::chrono::steady_clock::now();
  device->drawGlyphBatches(state.pageBatches);
  const auto recordEnd = std::chrono::steady_clock::now();

  for (const std::shared_ptr<Doc> &doc : state.docs) {
    doc->drawCaret(state, viewProjection, *caret);
  }

  // Whatever the program draws for itself: after the documents, so it can sit
  // over them, and before the notifications, which must be over everything.
  if (!frameContributors.empty()) {
    gleditor::FrameContext ctx{state, viewProjection, screenWidth, screenHeight,
                               timeline};
    for (auto *const contributor : frameContributors) {
      contributor->drawFrame(ctx);
    }
  }

  // Last, so that the overlay is on top: its pipeline does not depth test, so
  // submission order is what decides.
  toasts->expire(ToastOverlay::Clock::now());
  toasts->draw(state, screenWidth, screenHeight);

  // What was just drawn, said. Here rather than anywhere else because this is
  // the one place that has the documents, the caret and the camera at the same
  // time, and a rectangle on screen cannot be worked out without all three.
  //
  // Costs a comparison per source on a frame where nothing changed, which is
  // most of them.
  if (const auto &publisher = this->state->accessibility; publisher) {
    documents.observe(state, caret.get(), viewProjection, screenWidth,
                      screenHeight);
    publisher->rebuild(screenWidth, screenHeight);
    // Anything an assistive technology asked to be done to a document. It
    // arrived on the event thread and waited for this one, because moving a
    // caret is this thread's business.
    for (const auto &want : documents.takeWanted()) {
      if (want.document < state.docs.size()) {
        caret->placeAt(want.document, want.byteOffset);
      }
    }
  }

  // Ask what is under the cursor, or at the pixel --pick named. The read is
  // asynchronous, so this only queues it; the answer is collected below on a
  // later frame.
  //
  // Only settled frames are queried: a frame drawn while pages are still being
  // built would answer for a document that is not there yet, which for --pick
  // means reporting an empty tag and exiting.
  if (settled) {
    if (awaitingSettle) {
      // The frame this step's work was scheduled on has been and gone, and
      // this one is settled, so the work is done and the script may go on.
      awaitingSettle = false;
      awaitingStep   = false;
      nextStep++;
    } else if (!scriptFinished()) {
      advanceScript(state);
    } else if (awaitingStep) {
      // Waiting on a readback: nothing else may issue one, or the answer this
      // step is waiting for would be lost among the others.
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
    } else if (!this->state->scriptReportsPicks()) {
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
  if (settled && scriptFinished() && this->state->dumpAccessibility) {
    // Once, on the first settled frame, and after the rebuild above so that
    // what is printed is what would be sent rather than the frame before it.
    this->state->dumpAccessibility = false;
    if (const auto &publisher = this->state->accessibility; publisher) {
      std::cout << gleditor::a11y::Publisher::describe(publisher->snapshot());
    }
  }

  if (settled && !hasPendingWork() && scriptFinished() &&
      !this->state->screenshotPath.empty()) {
    writeScreenshot(device->captureColorTarget(), this->state->screenshotPath);
    this->state->screenshotPath.clear();
  }

  const auto end              = std::chrono::steady_clock::now();
  this->state->frameTimeDelta = end - start;

  // Only settled frames are measured, and only once every requested click has
  // been answered, so the sample covers the frame the editor actually steadies
  // into rather than one still waiting on a readback.
  if (settled && 0 != this->state->benchmarkFrames && scriptFinished()) {
    benchFrame.push_back(end - start);
    benchCollect.push_back(recordStart - collectStart);
    benchRecord.push_back(recordEnd - recordStart);
    benchBatches = state.pageBatches.size();
  }

  return this->state->alive;
}

void Renderer::reportBenchmark() const {
  if (benchFrame.empty()) {
    std::cout << "benchmark: no settled frames were measured\n";
    return;
  }
  const auto median = [](std::vector<std::chrono::nanoseconds> samples) {
    std::ranges::nth_element(samples, samples.begin() + (samples.size() / 2));
    return std::chrono::duration<double, std::milli>(
               samples[samples.size() / 2])
        .count();
  };
  const auto caps = device->capabilities();
  std::cout << std::format(
      "benchmark: {} frames, {} page draws, median frame {:.3f} ms, median "
      "collect {:.3f} ms, median record {:.3f} ms, parallel recording "
      "available: {} ({} thread(s))\n",
      benchFrame.size(), benchBatches, median(benchFrame), median(benchCollect),
      median(benchRecord), caps.parallelCommandRecording ? "yes" : "no",
      caps.recordingThreads);
  std::cout << std::format(
      "pages: {} considered, {} culled, {} coarse, {} detailed\n",
      lastDraw.pages, lastDraw.culled, lastDraw.coarse, lastDraw.detailed);
}

void Renderer::placeCaretFromPick(RenderState &state,
                                  const render::PickingResult &pick) {
  // Every outcome is reported, including the ones that place no caret. The
  // read is asynchronous, so a line that named only the offset could not be
  // lined up with the click that caused it -- and a silent outcome would drop
  // out of a sequence of clicks entirely, which is exactly the misreading this
  // is here to prevent.
  // Whatever the program drew for itself gets first refusal, because a tag it
  // wrote is a tag only it can read. One that claims the click has dealt with
  // it, and the caret stays where it was: clicking on a program's own drawing
  // is not clicking on the text behind it.
  for (auto *const observer : pickObservers) {
    if (observer->picked(pick, state)) {
      return;
    }
  }

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
  const auto &doc   = state.docs[pick.tag.docIndex];
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
      if (awaitingStep) {
        // The step that asked for this answer is done; the next one may go.
        awaitingStep = false;
        nextStep++;
      }
    } else if (awaitingStep && awaitingPick && awaitingPick->first == pick->x &&
               awaitingPick->second == pick->y) {
      std::cout << std::format(
          "pick {},{}: kind {} doc {} page {} cluster {} frac {:.3f}\n",
          pick->x, pick->y, pick->tag.kind, pick->tag.docIndex,
          pick->tag.pageIndex, pick->tag.clusterIndex, pick->tag.fraction);
      awaitingPick.reset();
      awaitingStep = false;
      nextStep++;
    }

    // Hovering, reported separately from the script and only when the script
    // named no pixels of its own: a run that asked about particular places
    // wants those answers and not a line about where the mouse is.
    if (!this->state->scriptReportsPicks() &&
        !pick->tag.sameObject(reportedPick)) {
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

/// Carry out the next step of the automation script, if the one before it has
/// finished. One step per settled frame at most: a step that queues a picking
/// read is not finished until the answer arrives, and a step that edits is not
/// finished until the reflow it caused has settled, which is what `settled`
/// already says.
void Renderer::advanceScript(RenderState &state) {
  if (scriptFinished() || awaitingStep) {
    return;
  }
  const auto &step = this->state->script[nextStep];
  using Kind       = AppState::AutomationStep::Kind;

  switch (step.kind) {
  case Kind::Pick:
    // Answered on a later frame; collectPickingResults() reports it and moves
    // the script on.
    awaitingPick = std::pair{step.x, step.y};
    awaitingStep = true;
    device->requestPickingTag(step.x, step.y);
    return;
  case Kind::Click:
    awaitingClick = std::pair{step.x, step.y};
    awaitingDrag  = false;
    awaitingStep  = true;
    device->requestPickingTag(step.x, step.y);
    return;
  case Kind::Command:
    // By name, so a script says what it means. The command queues its own
    // work, as it would if somebody had pressed the key.
    if (!this->state->runCommand || !this->state->runCommand(step.text)) {
      std::cerr << std::format("--do: no command called \"{}\"\n", step.text);
    }
    finishStepWhenSettled();
    return;
  case Kind::Press:
    if (nullptr == this->state->modal || !this->state->modal->grabbing()) {
      std::cerr << "--key with nothing to press it in; use --do first\n";
    } else {
      static_cast<void>(this->state->modal->keyPressed(step.key, step.mods));
    }
    finishStepWhenSettled();
    return;
  case Kind::Type:
    // Into whatever has the keyboard. A form is on screen because something
    // asked a question, and answering it is what typing means while it is up.
    if (nullptr != this->state->modal && this->state->modal->grabbing()) {
      this->state->modal->textTyped(step.text);
      finishStepWhenSettled();
      return;
    }
    if (!caret->active() || caret->documentIndex() >= state.docs.size()) {
      std::cerr << "--type with no caret to type at; use --click first\n";
    } else {
      // Read before insert() moves the caret past what it is about to place.
      const auto at  = caret->byteOffset();
      auto &document = *state.docs[caret->documentIndex()];
      document.insert(state, at, step.text, caret.get());
      if (0 != step.decorations && this->state->onDecoratedInsert) {
        this->state->onDecoratedInsert(
            document, at, static_cast<std::uint32_t>(step.text.size()),
            step.decorations);
      }
    }
    // An edit schedules a reflow, and this frame was judged settled before it
    // was made. So the step is not finished here: it finishes on the next
    // frame that really is settled, which is after the pages it changed have
    // been laid out again. Otherwise a screenshot would show the document
    // mid-edit and the step after it would act on one.
    finishStepWhenSettled();
    return;
  case Kind::Select:
    if (state.docs.empty()) {
      std::cerr << "--select with no document to select in\n";
    } else {
      caret->placeAt(0, step.from);
      caret->anchorSelection();
      caret->extendTo(step.to);
      std::cout << std::format("select: doc 0 [{},{})\n",
                               caret->selectionStart(), caret->selectionEnd());
    }
    finishStepWhenSettled();
    return;
  }
  nextStep++;
}

void Renderer::finishStepWhenSettled() {
  // Always a frame, never a test of whether work is pending: an edit may
  // reflow on the spot rather than through the queue, and either way this
  // frame was drawn before the edit was made. Waiting for the next settled
  // frame means waiting for one drawn after it -- and if a reflow was queued,
  // for one drawn after that, since a queued reflow is what unsettles a frame.
  awaitingStep   = true;
  awaitingSettle = true;
}

void AbstractRenderer::addSpanDecorator(
    gleditor::SpanDecorator *const decorator) {
  if (nullptr != decorator &&
      std::ranges::find(spanDecorators, decorator) == spanDecorators.end()) {
    spanDecorators.push_back(decorator);
  }
}

void AbstractRenderer::removeSpanDecorator(
    gleditor::SpanDecorator *const decorator) {
  std::erase(spanDecorators, decorator);
}

void AbstractRenderer::addFrameContributor(
    gleditor::FrameContributor *const contributor) {
  if (nullptr != contributor &&
      std::ranges::find(frameContributors, contributor) ==
          frameContributors.end()) {
    frameContributors.push_back(contributor);
  }
}

void AbstractRenderer::removeFrameContributor(
    gleditor::FrameContributor *const contributor) {
  std::erase(frameContributors, contributor);
}

void AbstractRenderer::addPickObserver(gleditor::PickObserver *const observer) {
  if (nullptr != observer &&
      std::ranges::find(pickObservers, observer) == pickObservers.end()) {
    pickObservers.push_back(observer);
  }
}

void AbstractRenderer::removePickObserver(
    gleditor::PickObserver *const observer) {
  std::erase(pickObservers, observer);
}

void Renderer::updateHighlights(RenderState &state) {
  // Rebuilt every frame rather than cached: a drag moves the selection each
  // frame anyway, and the table is a handful of entries -- one per page the
  // selection touches.
  highlights.clear();

  // The selection goes in first, and both reasons are properties of what
  // consumes this table. The fragment stage returns on the first span that
  // covers it, so an earlier entry wins where two overlap; and the device
  // keeps the first render::maxHighlightRanges and drops the rest, so an
  // earlier entry survives a table that fills up. A user who cannot see what
  // they have selected has lost track of something they were doing, where a
  // decoration that loses to it is only telling them something.
  if (caret->hasSelection() && caret->documentIndex() < state.docs.size()) {
    state.docs[caret->documentIndex()]->highlightsFor(
        caret->selectionStart(), caret->selectionEnd(), selectionColour,
        highlights);
  }

  if (!spanDecorators.empty()) {
    for (const auto &doc : state.docs) {
      decoratedSpans.clear();
      for (auto *const decorator : spanDecorators) {
        decorator->decorate(*doc, decoratedSpans);
      }
      for (const auto &span : decoratedSpans) {
        if (span.end > span.start) {
          doc->highlightsFor(span.start, span.end, span.colour, highlights);
        }
      }
    }
  }

  device->setHighlights(highlights);
}

void Renderer::applyTypedText(RenderState &state) {
  if (!caret->active()) {
    return;
  }
  std::string typed;
  {
    const std::scoped_lock locker(this->state->typedMutex);
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
  case RenderItem::Type::CloseDoc: {
    closeDoc(state, dynamic_cast<RenderItemCloseDoc &>(item).docIndex);
    break;
  }
  case RenderItem::Type::SaveDoc: {
    saveDoc(state, dynamic_cast<RenderItemSaveDoc &>(item).docIndex);
    break;
  }
  case RenderItem::Type::Resize: {
    const auto &resize = dynamic_cast<RenderItemResize &>(item);
    device->resize(resize.width, resize.height);
    break;
  }
  case RenderItem::Type::OpenDoc: {
    const auto &openItem = dynamic_cast<RenderItemOpenDoc &>(item);
    openDoc(state, *openItem.source, openItem.depthZ);
    break;
  }
  case RenderItem::Type::Run: {
    dynamic_cast<const RenderItemRun &>(item)();
    break;
  }
  case RenderItem::Type::RunState: {
    dynamic_cast<const RenderItemRunState &>(item)(state);
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
  if (this->state->noPresent) {
    device->setPresentEnabled(false);
  }

  int winW = 0;
  int winH = 0;
  SDL_GetWindowSizeInPixels(window.window, &winW, &winH);
  if (winW > 0 && winH > 0) {
    std::scoped_lock locker(this->state->view);
    this->state->view.screenWidth  = winW;
    this->state->view.screenHeight = winH;
  }

  RenderState state(device.get());
  toasts = std::make_unique<ToastOverlay>(device.get(),
                                          std::string(defaultFontName()));
  caret  = std::make_unique<Caret>(device.get());

  // What the library itself has to say about what is on screen. Registered
  // here rather than earlier because the overlay is one of them and it does
  // not exist until there is a device -- and before the first frame, which is
  // the first time anything asks.
  //
  // The documents come first: an assistive technology walks the tree in order,
  // and what is being read matters more than the chrome over it.
  if (const auto &publisher = this->state->accessibility; publisher) {
    publisher->addSource(&documents, gleditor::a11y::Ids::documents);
    publisher->addSource(toasts.get(), gleditor::a11y::Ids::notifications);
  }

  createPipeline(state);

  for (const auto &[severity, message] : this->state->requestedToasts) {
    toasts->post(severity, message, state);
  }

  const auto loopStart   = std::chrono::steady_clock::now();
  double timeToFirstPage = 0.0;
  bool firstPageRecorded = false;

  while (this->state->alive) {

    // Drain queued commands before drawing, so that work requested before this
    // thread started -- files named on the command line, for instance -- is
    // carried out rather than discarded on the first frame.
    while (auto item = renderQueue.pop()) {
      dispatch(state, *item);
    }

    reapFinishedDocLoads();

    for (auto &doc : state.docs) {
      if (!doc->isFullyLoaded()) {
        doc->buildPendingPages(state);
        if (!doc->isFullyLoaded()) {
          break;
        }
      }
    }

    const bool docsLoading = std::ranges::any_of(
        state.docs, [](const auto &doc) { return !doc->isFullyLoaded(); });

    // Everything queued has been carried out and every document has finished
    // loading, so this frame shows the finished result. That is the frame a
    // screenshot should capture, and the point at which --profile may quit.
    const bool settled = !hasPendingWork() && !docsLoading;

    // still want to update once even if we don't have anything in the render
    // queue
    if (!update(state, settled)) {
      break;
    }

    if (!firstPageRecorded && !state.pageBatches.empty()) {
      timeToFirstPage   = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - loopStart)
                              .count();
      firstPageRecorded = true;
      std::cout << std::format(
          "[TIMING] First page rendered: {:.2f} ms (docs in render: {})\n",
          timeToFirstPage, state.docs.size());
    }

    // Presenting is what paces this loop: it blocks until the display is ready
    // for another frame. Without it the loop redraws as fast as a core will
    // let it, which on a software rasteriser means taking every core there is
    // -- including the one the background document load needs in order to
    // finish, and this loop will not settle until it does. Yielding a little
    // per frame costs a capture nothing and leaves that work somewhere to run.
    if (this->state->noPresent) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // A measured run outlives --profile: the point is the steady state, which
    // the first settled frame is not part of.
    if (0 != this->state->benchmarkFrames) {
      if (benchFrame.size() >= this->state->benchmarkFrames) {
        reportBenchmark();
        this->state->alive = false;
        break;
      }
      continue;
    }

    // A pick takes a frame or two to come back, so profiling waits for every
    // requested query rather than exiting with one still outstanding.
    // Every step carried out and answered: the script is what this run was
    // for, so quitting before it finished would report on a document the
    // command line did not ask for.
    if (settled && this->state->profiling && scriptFinished()) {
      const auto completeRender =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - loopStart)
              .count();
      std::size_t totalPages = 0;
      for (const auto &doc : state.docs) {
        totalPages += doc->numPages();
      }
      std::cout << std::format("[TIMING] Complete render settled: {:.2f} ms "
                               "(docs: {}, total pages: {})\n",
                               completeRender, state.docs.size(), totalPages);
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
  // Motions point at outputs inside the documents, so they have to go before
  // the documents do.
  timeline.clear();
  fadingDocs.clear();
  state.docs.clear();
  caret.reset();
  toasts.reset();
  device->shutdown();
}

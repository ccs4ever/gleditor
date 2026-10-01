/**
 * @file views.cpp
 * @brief Document presentation and views coordinator for Xudu.
 */
#include "views.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/animation.hpp>
#include <gleditor/app.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/form.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/media.hpp>
#include <gleditor/media_stream.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/mimetype.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>

#include "common/ui/hypertime_graph.hpp"
#include "common/xanadu/config.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/swarm_catalog.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/transcopyright_crypto.hpp"
#include "common/xanadu/transcopyright_logic.hpp"
#include "xudu/beams.hpp"
#include "xudu/kinetic_tether_overlay.hpp"
#include "xudu/pouch_drawer.hpp"
#include "xudu/satelloid.hpp"
#include "xudu/session.hpp"
#include "xudu/wireframe_hull.hpp"

namespace xudu {

Views::Views(Session &aSession, RendererRef aRenderer, HypertimeMap &aMap,
             ImageOverlay &aImages, gleditor::Form &aForm, AppStateRef aState,
             std::shared_ptr<gleditor::DocumentSwitcher> aSwitcher)
    : session(aSession), renderer(std::move(aRenderer)), map(aMap),
      images(aImages), form(aForm), state(std::move(aState)),
      switcher(std::move(aSwitcher)) {}

Views::~Views() = default;

void Views::deviceReady(render::RenderDevice &device,
                        const render::PipelineDesc &documentPipeline) {
  device_       = &device;
  documentDesc_ = documentPipeline;
}

void Views::drawFrame(gleditor::FrameContext &ctx) {
  if (ctx.state.documentsVisible) frameForReading(ctx);
  if (pendingCamera_ && pendingCamera_()) {
    pendingCamera_ = {};
  }
}

void Views::frameForReading(const gleditor::FrameContext &ctx) {
  auto target = frameTarget_.lock();
  if (!target && !readingFramed_ && !ctx.state.docs.empty()) {
    target = ctx.state.docs.front();
  }
  if (!target || readableTextPx_ <= 0.0F) {
    return;
  }
  const auto &doc      = *target;
  const auto frame     = doc.pageFrame(0);
  const auto firstLine = doc.anchorFor(0);
  if (!frame || !firstLine) {
    return;
  }
  const float toWorld = glm::length(glm::vec3(frame->localToWorld[1]));
  std::scoped_lock locker(state->view);
  auto &view          = state->view;
  const auto distance = xanadu::readableCameraDistance(
      firstLine->height * toWorld, static_cast<float>(view.screenHeight),
      view.fov, readableTextPx_);
  if (!distance || view.screenWidth <= 0) {
    return;
  }
  const glm::vec3 topLeft(frame->localToWorld *
                          glm::vec4(frame->leftPx, frame->topPx, 0.0F, 1.0F));
  const glm::vec3 topRight(frame->localToWorld *
                           glm::vec4(frame->rightPx, frame->topPx, 0.0F, 1.0F));
  const float halfH = *distance * std::tan(glm::radians(view.fov) * 0.5F);
  const float halfW = halfH * static_cast<float>(view.screenWidth) /
                      static_cast<float>(view.screenHeight);
  const float x           = topRight.x - topLeft.x <= 2.0F * halfW
                                ? 0.5F * (topLeft.x + topRight.x)
                                : topLeft.x + halfW;
  const float chromeTopPx = std::max(ctx.chrome.top, ctx.settledChrome.top);
  const float worldPerPx = 2.0F * halfH / static_cast<float>(view.screenHeight);
  view.pos       = glm::vec3(x, topLeft.y - halfH + (chromeTopPx * worldPerPx),
                             topLeft.z + *distance);
  readingFramed_ = true;
  frameTarget_.reset();
}

void Views::keepInView(const Doc &doc, const std::uint32_t offset) {
  const auto anchor = doc.anchorFor(offset);
  if (!anchor) {
    return;
  }
  const auto found = doc.worldPoint(*anchor);
  if (!found) {
    return;
  }
  const auto point = *found;
  std::scoped_lock locker(state->view);
  auto &view = state->view;
  if (view.screenHeight <= 0 || view.pos.z <= point.z) {
    return;
  }
  constexpr float kComfort = 0.8F;
  const float halfH =
      (view.pos.z - point.z) * std::tan(glm::radians(view.fov) * 0.5F);
  const float halfW = halfH * static_cast<float>(view.screenWidth) /
                      static_cast<float>(view.screenHeight);
  const auto follow = [](float &camera, const float at, const float half) {
    const float reach = half * kComfort;
    if (at > camera + reach) {
      camera = at - reach;
    } else if (at < camera - reach) {
      camera = at + reach;
    }
  };
  follow(view.pos.x, point.x, halfW);
  follow(view.pos.y, point.y, halfH);
}

std::optional<Doc::Anchor>
Views::widgetRectFor(const Doc &doc, const std::uint32_t docOffset) const {
  for (const auto &widget : mediaWidgets) {
    if (auto rect = widget->rectFor(doc, docOffset)) {
      return rect;
    }
  }
  return std::nullopt;
}

void Views::showOnly(const MicroversionId &version,
                     const std::size_t storeIndex) {
  const auto count = session.views().size();
  for (std::size_t i = 0; i < count; i++) {
    renderer->push(RenderItemCloseDoc());
  }
  renderer->runWithState([this](RenderState &) { session.clearViews(); });
  showAlongside(version, 0.0F, storeIndex);
}

void Views::syncMediaWidgets(RenderState &rState) {
  for (const auto &old : mediaWidgets) {
    renderer->removeFrameContributor(old.get());
    renderer->removePickObserver(old.get());
    state->accessibility->removeSource(old.get());
  }
  mediaWidgets.clear();
  images.clear();
  for (std::size_t dIdx = 0;
       dIdx < rState.docs.size() && dIdx < session.views().size(); ++dIdx) {
    if (!rState.docs[dIdx]) {
      continue;
    }
    const auto &vInfo = session.views()[dIdx];
    const auto &st    = session.store(vInfo.storeIndex);
    const auto spans  = session.mediaSpansFor(vInfo.version, vInfo.storeIndex);
    for (const auto &mSpan : spans) {
      const auto bytes = st.read(
          xudu::PrimediaSpan{.scroll = mSpan.span.scroll,
                             .start  = mSpan.span.start - mSpan.containerOffset,
                             .length = mSpan.containerLength});
      if (mSpan.isImage) {
        const auto id = std::format("{}:{}:{}", mSpan.span.scroll,
                                    mSpan.span.start - mSpan.containerOffset,
                                    mSpan.containerLength);
        images.place(rState.docs[dIdx], mSpan.docOffset, id,
                     std::span<const std::uint8_t>(
                         reinterpret_cast<const std::uint8_t *>(bytes.data()),
                         bytes.size()),
                     gleditor::MimeType(mSpan.mime));
        continue;
      }
      auto widget = std::make_shared<gleditor::MediaWidget>("Sans 11");
      if (nullptr != device_) {
        widget->deviceReady(*device_, documentDesc_);
      }
      auto stream = std::make_shared<gleditor::MemoryMediaStream>(bytes);
      std::ignore = widget->loadFragment(
          gleditor::MediaResource::fromStream(stream, mSpan.label),
          gleditor::ByteRange{.start  = mSpan.containerOffset,
                              .length = mSpan.span.length},
          mSpan.containerLength);
      widget->setTitle(mSpan.label);
      widget->attachToDocument(rState.docs[dIdx], mSpan.docOffset);
      widget->setSize(mSpan.widgetWidth, mSpan.widgetHeight);
      widget->setVisible(true);
      renderer->addFrameContributor(widget.get());
      renderer->addPickObserver(widget.get());
      state->accessibility->addSource(widget.get());
      mediaWidgets.push_back(std::move(widget));
    }
  }
}

void Views::showAlongside(const MicroversionId &version, const float depthZ,
                          const std::size_t storeIndex,
                          const std::uint32_t focusedBirth) {
  renderer->push(RenderItemOpenDoc(
      session.sourceFor(version, storeIndex, focusedBirth), depthZ));
  renderer->runWithState(
      [this, version, storeIndex, focusedBirth](RenderState &rState) {
        if (rState.docs.empty()) {
          return;
        }
        primaryDocument_ = rState.docs.front();
        rState.docs.back()->addObserver(&session);
        session.viewOpened(version, storeIndex, focusedBirth);
        map.setCurrent(session.views().front().version);
        syncMediaWidgets(rState);
      });
}

std::optional<glm::mat4> Views::presentationTransform() const {
  auto document = presentationAnchor_.lock();
  if (!document) {
    document = primaryDocument_.lock();
  }
  if (!document) {
    return std::nullopt;
  }
  const auto frame = document->pageFrame(0);
  if (!frame) {
    return std::nullopt;
  }
  return frame->localToWorld *
         glm::translate(
             glm::mat4{1.0F},
             glm::vec3{frame->rightPx + frame->marginPx, 0.0F, 0.0F});
}

void Views::swingBackToSpan(const PouchItem &item) {
  renderer->runWithState([this, item](RenderState &rState) {
    if (session.views().empty()) {
      return;
    }
    std::optional<std::size_t> foundDocIdx;
    for (std::size_t i = 0; i < session.views().size(); ++i) {
      if (session.views()[i].version == item.originVersion) {
        foundDocIdx = i;
        break;
      }
    }

    if (!foundDocIdx.has_value()) {
      showAlongside(item.originVersion, 0.0F, 0);
      foundDocIdx = session.views().size() - 1;
    }

    const auto docIdx = *foundDocIdx;
    if (onionSkinMode_) {
      activeOnionIdx_ = docIdx;
      arrangeOnionSkin(rState);
    }

    if (docIdx >= session.views().size()) {
      return;
    }

    const auto &st  = session.store(session.views()[docIdx].storeIndex);
    const auto ver  = st.rebuild(item.originVersion);
    const auto occs = ver.occurrencesOf(item.span);

    if (!occs.empty()) {
      const auto &occ   = occs.front();
      auto *const caret = renderer->editCaret();
      if (caret) {
        caret->placeAt(static_cast<std::uint32_t>(docIdx), occ.start);
        caret->extendTo(occ.end);
      }
    }
  });
}

void Views::focusSpan(const zigzag::CellRef /*cell*/,
                      const PrimediaSpan &span) {
  renderer->runWithState([this, span](RenderState &rState) {
    if (session.views().empty()) {
      return;
    }
    if (span.length > 0) {
      for (std::size_t docIdx = 0; docIdx < session.views().size(); ++docIdx) {
        const auto &vInfo = session.views()[docIdx];
        const auto &st    = session.store(vInfo.storeIndex);
        const auto ver    = st.rebuild(vInfo.version);
        const auto occs   = ver.occurrencesOf(span);
        if (!occs.empty()) {
          const auto &occ   = occs.front();
          auto *const caret = renderer->editCaret();
          if (caret) {
            caret->placeAt(static_cast<std::uint32_t>(docIdx), occ.start);
            caret->extendTo(occ.end);
          }
          if (docIdx < rState.docs.size() && rState.docs[docIdx]) {
            const auto &doc = rState.docs[docIdx];
            if (const auto anch = doc->anchorFor(occ.start)) {
              if (const auto wp =
                      doc->worldPoint(anch->pageIndex, anch->x, anch->y)) {
                if (state) {
                  std::scoped_lock locker(state->view);
                  state->view.pos.x = wp->x;
                  state->view.pos.y = wp->y;
                }
              }
            }
          }
          return;
        }
      }
    }

    for (std::size_t docIdx = 0; docIdx < session.views().size(); ++docIdx) {
      const auto &vInfo = session.views()[docIdx];
      const auto &st    = session.store(vInfo.storeIndex);
      for (const auto &[linkId, link] : st.links()) {
        for (const auto &lSpan : link.left) {
          const auto occs = st.rebuild(vInfo.version).occurrencesOf(lSpan);
          if (!occs.empty()) {
            auto *const caret = renderer->editCaret();
            if (caret) {
              caret->placeAt(static_cast<std::uint32_t>(docIdx),
                             occs.front().start);
              caret->extendTo(occs.front().end);
            }
            return;
          }
        }
      }
    }
  });
}

void Views::focusSpan(const std::size_t docIndex, const std::uint32_t charStart,
                      const std::uint32_t charEnd) {
  renderer->runWithState(
      [this, docIndex, charStart, charEnd](RenderState &rState) {
        if (docIndex >= session.views().size()) {
          return;
        }
        auto *const caret = renderer->editCaret();
        if (caret) {
          caret->placeAt(static_cast<std::uint32_t>(docIndex), charStart);
          caret->extendTo(charEnd);
        }
        if (docIndex < rState.docs.size() && rState.docs[docIndex]) {
          const auto &doc = rState.docs[docIndex];
          if (const auto anch = doc->anchorFor(charStart)) {
            if (const auto wp =
                    doc->worldPoint(anch->pageIndex, anch->x, anch->y)) {
              if (state) {
                std::scoped_lock locker(state->view);
                state->view.pos.x = wp->x;
                state->view.pos.y = wp->y;
              }
            }
          }
        }
      });
}

void Views::focusContent(std::vector<PrimediaSpan> content) {
  renderer->runWithState([this, content = std::move(content)](RenderState &) {
    for (std::size_t docIdx = 0; docIdx < session.views().size(); ++docIdx) {
      const auto found = xanadu::contentOccurrences(
          session.views()[docIdx].pieces.pieces(), content);
      if (!found.empty()) {
        focusSpan(docIdx, found.front().start, found.front().end);
        return;
      }
    }
    GLEDITOR_LOG_DEBUG("xudu.links",
                       "activated cell content is in no open document");
  });
}

void Views::back() {
  renderer->runWithState([this](RenderState &) {
    if (session.views().empty()) {
      return;
    }
    const auto here = session.views().front().version;
    if (here.isZero()) {
      std::cout << "xudu: already at the null document\n";
      return;
    }
    const auto there = here.parent();
    std::cout << "xudu: " << here.str() << " -> " << there.str() << "\n";
    showOnly(there, session.views().front().storeIndex);
  });
}

void Views::forward() {
  renderer->runWithState([this](RenderState &) {
    if (session.views().empty()) {
      return;
    }
    const auto sIdx     = session.views().front().storeIndex;
    const auto here     = session.views().front().version;
    const auto children = session.store(sIdx).children(here);
    if (children.empty()) {
      std::cout << "xudu: " << here.str() << " has no successor\n";
      return;
    }
    std::cout << "xudu: " << here.str() << " -> " << children.front().str();
    if (children.size() > 1) {
      std::cout << " (of " << children.size() << " futures)";
    }
    std::cout << "\n";
    showOnly(children.front(), sIdx);
  });
}

void Views::scrubHistory(const bool backward) {
  renderer->runWithState([this, backward](RenderState &rState) {
    if (session.views().empty() || rState.docs.empty()) {
      return;
    }
    auto *const caret = renderer->editCaret();
    const auto docIdx = (nullptr != caret && caret->active() &&
                         caret->documentIndex() < rState.docs.size())
                            ? caret->documentIndex()
                            : 0U;

    auto &doc           = *rState.docs[docIdx];
    const bool scrubbed = backward ? session.scrubBackward(docIdx, doc, 1)
                                   : session.scrubForward(docIdx, doc, 1);
    if (scrubbed) {
      syncMediaWidgets(rState);
      const auto newVer = session.versionOf(docIdx);
      const auto hist   = session.historyOf(docIdx);
      const auto it     = std::ranges::find(hist, newVer);
      const auto step =
          (it != hist.end())
              ? static_cast<std::size_t>(std::distance(hist.begin(), it) + 1)
              : 0U;
      std::cout << "xudu: hypertime scrub -> " << newVer.str() << " (" << step
                << "/" << hist.size() << ")\n";
    }
  });
}

void Views::deleteSelection() {
  withCaret([](RenderState &rState, const Where &where, Caret *caret) {
    auto &doc  = *rState.docs[where.doc];
    auto start = where.start;
    if (!where.hasRange) {
      start = gleditor::stepCharacter(doc.contents(), where.start, false);
    }
    if (start < where.end) {
      doc.erase(rState, start, where.end - start, caret);
    }
  });
}

void Views::deleteForward() {
  withCaret([](RenderState &rState, const Where &where, Caret *caret) {
    auto &doc = *rState.docs[where.doc];
    auto end  = where.end;
    if (!where.hasRange) {
      end = gleditor::stepCharacter(doc.contents(), where.end, true);
    }
    if (where.start < end) {
      doc.erase(rState, where.start, end - where.start, caret);
    }
  });
}

void Views::moveCaret(const gleditor::CaretMotion motion, const bool extend) {
  withCaret([this, motion, extend](RenderState &rState, const Where &where,
                                   Caret *caret) {
    const auto &doc = *rState.docs[where.doc];
    std::uint32_t target{};
    if (!extend && where.hasRange && gleditor::CaretMotion::Left == motion) {
      target = where.start;
    } else if (!extend && where.hasRange &&
               gleditor::CaretMotion::Right == motion) {
      target = where.end;
    } else {
      target = gleditor::caretTarget(doc, caret->byteOffset(), motion);
    }
    if (extend) {
      if (!caret->hasSelection()) {
        caret->anchorSelection();
      }
      caret->extendTo(target);
    } else {
      caret->placeAt(where.doc, target);
    }
    keepInView(doc, target);
  });
}

void Views::transcludeSelection() {
  withCaret([this](RenderState &, const Where &where, Caret *) {
    if (!where.hasRange) {
      std::cout << "xudu: select something to transclude first\n";
      return;
    }
    const auto from   = session.versionOf(where.doc);
    const auto sIdx   = session.storeIndexOf(where.doc);
    const auto quoted = session.store(sIdx).transclude(
        MicroversionId{}, 0, from, where.start, where.end - where.start);
    std::cout << "xudu: transcluded [" << where.start << "," << where.end
              << ") of " << from.str() << " into " << quoted.str() << "\n";
    showAlongside(quoted, 0.0F, sIdx);
  });
}

void Views::linkSelection() {
  withCaret([this](RenderState &, const Where &where, Caret *) {
    if (!where.hasRange) {
      std::cout << "xudu: select something to xanalink first\n";
      return;
    }
    const auto version = session.versionOf(where.doc);
    const auto sIdx    = session.storeIndexOf(where.doc);
    auto spans         = session.store(sIdx).rebuild(version).spansFor(
        where.start, where.end - where.start);

    if (!pending) {
      pending = Pending{.doc   = where.doc,
                        .start = where.start,
                        .end   = where.end,
                        .spans = std::move(spans)};
      std::cout << "xudu: xanalink from doc " << where.doc << " ["
                << where.start << "," << where.end
                << ") -- select the other end and press ctrl-l again\n";
      return;
    }

    xudu::Link link;
    link.type        = xudu::LinkType::Comment;
    link.owner       = "you";
    link.left        = std::move(pending->spans);
    link.right       = std::move(spans);
    const auto after = session.addLink(where.doc, link);
    std::cout << "xudu: link doc " << pending->doc << " [" << pending->start
              << "," << pending->end << ") -> doc " << where.doc << " ["
              << where.start << "," << where.end << ") at " << after.str()
              << "\n";
    pending.reset();
  });
}

void Views::cancelLink() {
  renderer->runWithState([this](RenderState &) {
    if (!pending) {
      std::cout << "xudu: no link waiting for its other end\n";
      return;
    }
    std::cout << "xudu: dropped the link begun at doc " << pending->doc << " ["
              << pending->start << "," << pending->end << ")\n";
    pending.reset();
  });
}

void Views::addCellToPendingLink(const std::span<const PrimediaSpan> content) {
  if (!pending) {
    std::cout << "xudu: select a document passage with Ctrl+L first\n";
    return;
  }
  if (content.empty()) {
    std::cout << "xudu: focused cell has no content to link\n";
    return;
  }
  pending->right.insert(pending->right.end(), content.begin(), content.end());
  ++pending->rightCells;
  std::cout << "xudu: added cell " << pending->rightCells
            << " to the pending link; choose another or finish the link\n";
}

void Views::finishCellLink() {
  if (!pending || pending->right.empty()) {
    std::cout << "xudu: select a passage and add at least one cell first\n";
    return;
  }
  xudu::Link link;
  link.type        = xudu::LinkType::Comment;
  link.owner       = "you";
  link.left        = std::move(pending->spans);
  link.right       = std::move(pending->right);
  const auto after = session.addLink(0, std::move(link));
  std::cout << "xudu: linked document passage to " << pending->rightCells
            << " cell(s) at " << after.str() << "\n";
  pending.reset();
}

void Views::publishCurrent(const std::string &salt) {
  renderer->runWithState([this, salt](RenderState &) {
    if (session.views().empty()) {
      std::cout << "xudu: nothing open to publish\n";
      state->showDialog(render::DiagnosticSeverity::Warning,
                        "Nothing to publish",
                        "No document is open. Open one, and what is under "
                        "the caret is what gets published.");
      return;
    }
    auto *const caret   = renderer->editCaret();
    const auto which    = nullptr != caret && caret->active() &&
                               caret->documentIndex() < session.views().size()
                              ? caret->documentIndex()
                              : 0U;
    const auto version  = session.versionOf(which);
    const auto storeIdx = session.storeIndexOf(which);
    const auto who      = session.author();
    const auto where    = session.publishedDir(storeIdx);

    using Field = gleditor::Form::Field;
    using Kind  = gleditor::Form::Kind;

    Field keys;
    keys.label    = "Signing key";
    keys.value    = {};
    keys.hint     = "no signing key in the keyring";
    keys.required = false;
    keys.kind     = Kind::Choice;

    for (const auto &key : signingKeys(session.settings().signing())) {
      keys.options.push_back(key.describe());
      keys.optionValues.push_back(key.fingerprint);
      const bool wanted = who.gpgKey.empty()
                              ? key.preferred
                              : key.fingerprint.ends_with(who.gpgKey) ||
                                    key.identity.contains(who.gpgKey);
      if (wanted) {
        keys.chosen = keys.options.size() - 1;
      }
    }
    if (keys.options.empty()) {
      std::cout << "xudu: no signing key in the keyring\n";
      state->showDialog(
          render::DiagnosticSeverity::Error, "No signing key",
          "The keyring has no secret key to sign an authorship record with, "
          "and a document is signed before it is sealed. Make one with `gpg "
          "--quick-generate-key`, or point gpg_home in " +
              xudu::configPath() + " at the keyring that holds yours.");
      return;
    }

    std::vector<Field> asked;
    Field fName;
    fName.label = "Name";
    fName.value = salt.empty() ? std::string{"document"} : salt;
    fName.hint  = "one word; publishing again under it is a further state of "
                  "this document";
    fName.required = true;
    asked.push_back(std::move(fName));

    Field fTitle;
    fTitle.label    = "Title";
    fTitle.value    = salt.empty() ? std::string{"document"} : salt;
    fTitle.hint     = "what this document is called";
    fTitle.required = true;
    asked.push_back(std::move(fTitle));

    Field fAuthor;
    fAuthor.label    = "Author";
    fAuthor.value    = who.name;
    fAuthor.hint     = "who is publishing this";
    fAuthor.required = true;
    asked.push_back(std::move(fAuthor));

    Field fEmail;
    fEmail.label    = "Email";
    fEmail.value    = who.email;
    fEmail.hint     = "how to reach them";
    fEmail.required = true;
    asked.push_back(std::move(fEmail));

    asked.push_back(std::move(keys));

    Field fPass;
    fPass.label    = "Passphrase";
    fPass.value    = {};
    fPass.hint     = "only if the agent is not holding it";
    fPass.required = false;
    fPass.kind     = Kind::Secret;
    asked.push_back(std::move(fPass));

    Field toggle;
    toggle.label          = "";
    toggle.value          = {};
    toggle.hint           = {};
    toggle.required       = false;
    toggle.kind           = Kind::Toggle;
    toggle.revealsSecrets = true;
    asked.push_back(std::move(toggle));

    Field fRights;
    fRights.label    = "Rights";
    fRights.value    = {};
    fRights.hint     = "how others may use this; optional";
    fRights.required = false;
    asked.push_back(std::move(fRights));

    Field fNote;
    fNote.label    = "Note";
    fNote.value    = {};
    fNote.hint     = "anything else worth recording; optional";
    fNote.required = false;
    asked.push_back(std::move(fNote));

    form.open(
        "Publish " + version.str(),
        "Signed as an authorship record, then sealed into " + where,
        std::move(asked),
        [this, version, which, storeIdx](const std::vector<Field> &answers) {
          publishAnswers(version, which, storeIdx, answers);
        });
  });
}

void Views::publishAnswers(const MicroversionId &version,
                           const std::uint32_t which,
                           const std::size_t storeIdx,
                           const std::vector<gleditor::Form::Field> &answers) {
  Session::PublishRequest request;
  request.salt          = answers[0].answer();
  request.title         = answers[1].answer();
  request.author.name   = answers[2].answer();
  request.author.email  = answers[3].answer();
  request.author.gpgKey = answers[4].answer();
  request.passphrase    = answers[5].answer();
  if (!answers[7].answer().empty()) {
    request.extra.emplace_back("rights", answers[7].answer());
  }
  if (!answers[8].answer().empty()) {
    request.extra.emplace_back("note", answers[8].answer());
  }

  renderer->runWithState([this, version, which, storeIdx,
                          request](RenderState &) {
    try {
      const auto path = session.publishDocument(version, request, storeIdx);
      std::cout << "xudu: published doc " << which << " as " << path << "\n";
    } catch (const std::exception &err) {
      std::cout << "xudu: cannot publish: " << err.what() << "\n";
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Could not publish " + version.str(), err.what());
    }
  });
}

void Views::saveCurrent() {
  renderer->runWithState([this](RenderState &) {
    if (session.views().empty()) {
      session.saveAll();
      return;
    }
    auto *const caret = renderer->editCaret();
    auto which        = switcher ? switcher->activeDocIndex() : 0U;
    if (nullptr != caret && caret->active() &&
        caret->documentIndex() < session.views().size()) {
      which = caret->documentIndex();
    }
    if (which >= session.views().size()) {
      session.saveAll();
      return;
    }
    const auto storeIdx = session.storeIndexOf(which);
    if (session.isTemporaryStore(storeIdx)) {
      using Field         = gleditor::Form::Field;
      namespace fs        = std::filesystem;
      std::string curDir  = fs::current_path().string();
      std::string defName = "doc_" + std::to_string(storeIdx) + ".xanadoc";

      std::vector<Field> fields;
      Field fFolder;
      fFolder.label    = "Folder";
      fFolder.value    = curDir;
      fFolder.hint     = "directory where the xanadoc folder will live";
      fFolder.required = true;
      fields.push_back(std::move(fFolder));

      Field fName;
      fName.label    = "Name";
      fName.value    = defName;
      fName.hint     = "name of the xanadoc folder";
      fName.required = true;
      fields.push_back(std::move(fName));

      form.open(
          "Preserve Temporary Xanadoc",
          "Designate a permanent directory and name for this temporary store",
          std::move(fields),
          [this, storeIdx](const std::vector<Field> &answers) {
            preserveAnswers(storeIdx, answers);
          });
    } else {
      session.save(storeIdx);
      std::cout << "xudu: saved to " << session.path(storeIdx) << "\n";
    }
  });
}

void Views::preserveAnswers(const std::size_t storeIdx,
                            const std::vector<gleditor::Form::Field> &answers) {
  namespace fs             = std::filesystem;
  const std::string folder = answers[0].answer();
  const std::string name   = answers[1].answer();
  const fs::path targetDir = fs::path(folder) / name;

  renderer->runWithState([this, storeIdx, targetDir](RenderState &) {
    try {
      namespace fs = std::filesystem;
      fs::create_directories(targetDir);
      auto &st = session.store(storeIdx);
      st.save(targetDir.string());
      session.setStorePath(storeIdx, targetDir.string(), false);
      std::cout << "xudu: preserved temporary store to " << targetDir.string()
                << "\n";
    } catch (const std::exception &err) {
      std::cout << "xudu: cannot preserve store: " << err.what() << "\n";
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Could not preserve xanadoc", err.what());
    }
  });
}

void Views::closeDocument(const std::uint32_t docIndex) {
  session.flushUncommitted(docIndex);
  renderer->push(RenderItemCloseDoc(docIndex));
  renderer->runWithState([this, docIndex](RenderState &rState) {
    session.viewClosed(docIndex);
    if (!session.views().empty()) {
      map.setCurrent(session.views().front().version);
    }
    syncMediaWidgets(rState);
  });
}

void Views::closeActive() {
  renderer->runWithState([this](RenderState &rState) {
    if (rState.docs.empty()) {
      return;
    }
    auto *const caret = renderer->editCaret();
    auto which        = switcher ? switcher->activeDocIndex() : 0U;
    if (nullptr != caret && caret->active() &&
        caret->documentIndex() < rState.docs.size()) {
      which = caret->documentIndex();
    }
    if (which < rState.docs.size()) {
      closeDocument(which);
    }
  });
}

void Views::activateNewest() {
  renderer->runWithState([this](RenderState &rState) {
    if (!rState.docs.empty()) {
      activateDocument(rState,
                       static_cast<std::uint32_t>(rState.docs.size() - 1));
    }
  });
}

void Views::activateDocument(RenderState &rState, const std::uint32_t index) {
  if (index >= rState.docs.size() || !rState.docs[index]) {
    return;
  }
  if (switcher) {
    switcher->setActiveDocIndex(index);
  }
  if (auto *const caret = renderer->editCaret(); caret) {
    caret->placeAt(index, 0);
  }
  frameTarget_ = rState.docs[index];
}

std::size_t Views::newDocument() {
  const auto storeIndex = session.createNewStore("");
  showAlongside(MicroversionId{}, 0.0F, storeIndex);
  activateNewest();
  std::cout << "xudu: created new sovereign document (store " << storeIndex
            << ")\n";
  return storeIndex;
}

xanadu::ReadingPlace Views::currentPlace() const {
  xanadu::ReadingPlace place;
  const auto *const caret = renderer->editCaret();
  for (std::size_t i = 0; i < session.views().size(); ++i) {
    const auto &view = session.views()[i];
    xanadu::DocumentPlace document{.storePath = session.path(view.storeIndex),
                                   .version   = view.version.str()};
    if (nullptr != caret && caret->active() && caret->documentIndex() == i) {
      document.caret  = caret->byteOffset();
      document.anchor = !caret->hasSelection() ? caret->byteOffset()
                        : caret->byteOffset() == caret->selectionStart()
                            ? caret->selectionEnd()
                            : caret->selectionStart();
      place.active    = i;
    }
    place.documents.push_back(std::move(document));
  }
  std::scoped_lock locker(state->view);
  place.camera = std::array<double, 4>{state->view.pos.x, state->view.pos.y,
                                       state->view.pos.z, state->view.fov};
  return place;
}

void Views::restorePlace(const xanadu::ReadingPlace &place,
                         std::vector<std::optional<std::uint32_t>> opened,
                         std::vector<std::uint32_t> lengths) {
  renderer->runWithState([this, place, opened = std::move(opened),
                          lengths = std::move(lengths)](RenderState &rState) {
    if (place.camera) {
      // The saved view, not a fresh reading framing of the first page.
      readingFramed_ = true;
      frameTarget_.reset();
      std::scoped_lock locker(state->view);
      const auto &[x, y, z, fov] = *place.camera;
      state->view.pos = glm::vec3(static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(z));
      state->view.fov = static_cast<float>(fov);
    }
    if (!place.active || *place.active >= opened.size() ||
        !opened[*place.active]) {
      return;
    }
    const auto index = *opened[*place.active];
    if (index >= rState.docs.size()) {
      return;
    }
    const auto &saved = place.documents[*place.active];
    const auto length = *place.active < lengths.size() ? lengths[*place.active]
                                                       : std::uint32_t{0};
    if (switcher) {
      switcher->setActiveDocIndex(index);
    }
    if (auto *const caret = renderer->editCaret(); caret) {
      caret->placeAt(index, std::min(saved.anchor, length));
      if (saved.anchor != saved.caret) {
        caret->anchorSelection();
        caret->extendTo(std::min(saved.caret, length));
      }
    }
  });
}

void Views::spawnTranscludedDocument(const TetherPayload &payload,
                                     const float /*screenX*/,
                                     const float /*screenY*/) {
  if (payload.originCharEnd <= payload.originCharStart) {
    return;
  }
  const auto len        = payload.originCharEnd - payload.originCharStart;
  const auto sIdx       = PouchOriginKind::Document == payload.originKind &&
                            payload.originDocIndex < session.views().size()
                              ? session.storeIndexOf(payload.originDocIndex)
                              : std::size_t{0};
  const auto spawnedVer = session.store(sIdx).transclude(
      MicroversionId{}, 0, payload.originVersion, payload.originCharStart, len);
  showAlongside(spawnedVer, 0.0F, sIdx);
  activateNewest();
  std::cout << "xudu: spawned transcluded document version " << spawnedVer.str()
            << " from origin version " << payload.originVersion.str() << " ["
            << payload.originCharStart << ", " << payload.originCharEnd
            << ")\n";
}

void Views::summonPublication(const PublicationEntry &entry) {
  const auto storeIndex = session.createNewStore("");
  auto &st              = session.store(storeIndex);

  std::string content;
  content.reserve(entry.title.size() + entry.authorName.size() +
                  entry.abstractText.size() + 256);
  content.append("# ");
  content.append(entry.title);
  content.append("\n\nAuthor: ");
  content.append(entry.authorName);
  content.append("\nSwarm URI: ");
  content.append(entry.bep46Uri);
  content.append("\nInfoHash: ");
  content.append(entry.infoHash);
  content.append("\n\n");
  content.append(entry.abstractText);
  if (entry.hasTranscopyright) {
    content.append("\n\n[Transcopyright Active: ");
    content.append(entry.transcopyrightTerms);
    content.append("]\n");
  } else {
    content.append("\n\n[Merkle Verified Docuverse Publication]\n");
  }

  const auto ver = st.insert(MicroversionId{}, 0, content);
  showAlongside(ver, 0.0F, storeIndex);
  if (wireframeOverlay_ && !session.views().empty()) {
    const auto newDocIndex = session.views().size() - 1;
    wireframeOverlay_->startLoading(newDocIndex, entry.title, entry.infoHash,
                                    16);
    wireframeOverlay_->updateProgress(newDocIndex, 12);
  }
  activateNewest();
  std::cout << "xudu: summoned publication '" << entry.title
            << "' into 3D space (store " << storeIndex << ")\n";
}

void Views::insertSpanAtCaret(const PrimediaSpan &span) {
  withCaret([this, span](RenderState &rState, const Where &where, Caret *) {
    insertSpanAt(rState, where.doc, where.start, span);
  });
}

void Views::transcludeSpansAtCaret(std::vector<PrimediaSpan> spans) {
  withCaret([this, spans = std::move(spans)](RenderState &rState,
                                             const Where &where, Caret *) {
    auto at = where.start;
    for (const auto &span : spans) {
      insertSpanAt(rState, where.doc, at, span);
      at += static_cast<std::uint32_t>(span.length);
    }
  });
}

void Views::insertSpanAt(RenderState &rState, const std::uint32_t doc,
                         const std::uint32_t at, const PrimediaSpan &span) {
  if (doc >= rState.docs.size() || 0 == span.length) {
    return;
  }
  session.flushUncommitted(doc);
  const auto prod = session.insertSpan(doc, at, span);
  if (const auto src = session.sourceFor(prod, session.storeIndexOf(doc))) {
    rState.docs[doc]->load(*src);
    syncMediaWidgets(rState);
  }
  if (auto *const caret = renderer->editCaret(); caret) {
    caret->placeAt(doc, at + static_cast<std::uint32_t>(span.length));
  }
  std::cout << "xudu: transcluded " << span.length << " bytes into doc " << doc
            << " at " << at << "\n";
}

void Views::insertPageBreak(const std::uint32_t docIndex,
                            const std::uint32_t charOffset) {
  renderer->runWithState([this, docIndex, charOffset](RenderState &rState) {
    if (docIndex >= rState.docs.size()) {
      return;
    }
    const auto prod = session.insertBreak(docIndex, charOffset);
    if (const auto src =
            session.sourceFor(prod, session.storeIndexOf(docIndex))) {
      rState.docs[docIndex]->load(*src);
      syncMediaWidgets(rState);
    }
    std::cout << "xudu: page break inserted at doc " << docIndex << " offset "
              << charOffset << "\n";
  });
}

void Views::insertPageBreakAtCaret() {
  withCaret([this](RenderState &rState, const Where &where, Caret *) {
    if (where.doc >= rState.docs.size()) {
      return;
    }
    const auto prod = session.insertBreak(where.doc, where.start);
    if (const auto src =
            session.sourceFor(prod, session.storeIndexOf(where.doc))) {
      rState.docs[where.doc]->load(*src);
      syncMediaWidgets(rState);
    }
    std::cout << "xudu: page break inserted at doc " << where.doc << " offset "
              << where.start << "\n";
  });
}

void Views::exportOsmic() {
  renderer->runWithState([this](RenderState &) {
    if (session.views().empty()) {
      session.saveOsmicTextAll();
      return;
    }
    auto *const caret   = renderer->editCaret();
    const auto which    = (nullptr != caret && caret->active() &&
                        caret->documentIndex() < session.views().size())
                              ? caret->documentIndex()
                              : 0U;
    const auto storeIdx = session.storeIndexOf(which);
    session.store(storeIdx).saveOsmicText(session.path(storeIdx));
    std::cout << "xudu: exported osmic text for doc " << which << " ("
              << session.path(storeIdx) << ")\n";
  });
}

void Views::importFile(const std::string &filePath) {
  renderer->runWithState([this, filePath](RenderState &rState) {
    if (session.views().empty()) {
      return;
    }
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
      std::cout << "xudu: cannot read import file: " << filePath << "\n";
      return;
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    auto *const caret   = renderer->editCaret();
    const auto docIdx   = (nullptr != caret && caret->active() &&
                         caret->documentIndex() < session.views().size())
                              ? caret->documentIndex()
                              : 0U;
    const auto at       = (nullptr != caret && caret->active() &&
                     caret->documentIndex() == docIdx)
                              ? caret->byteOffset()
                              : 0U;
    const auto detected = gleditor::MimeDetector::detectFile(filePath);
    const auto mime =
        detected.empty() ? "text/plain;charset=utf-8" : detected.essence();
    const auto prod = session.insertMedia(docIdx, at, bytes, mime, filePath);
    if (docIdx < rState.docs.size()) {
      if (const auto src =
              session.sourceFor(prod, session.storeIndexOf(docIdx))) {
        rState.docs[docIdx]->load(*src);
        syncMediaWidgets(rState);
      }
    }
    std::cout << "xudu: imported file " << filePath << " into doc " << docIdx
              << " at offset " << at << " -> version " << prod.str() << "\n";
  });
}

void Views::setWireframeOverlay(WireframeHullOverlay *overlay) noexcept {
  wireframeOverlay_ = overlay;
}

void Views::unlockTranscopyright(const std::size_t storeIdx,
                                 const PrimediaSpan &span) {
  if (session.unlockTranscopyright(storeIdx, span)) {
    renderer->runWithState([this, storeIdx](RenderState &rState) {
      for (std::size_t dIdx = 0;
           dIdx < rState.docs.size() && dIdx < session.views().size(); ++dIdx) {
        if (!rState.docs[dIdx]) {
          continue;
        }
        const auto &vInfo = session.views()[dIdx];
        if (vInfo.storeIndex == storeIdx) {
          const auto src = session.sourceFor(vInfo.version, vInfo.storeIndex);
          rState.docs[dIdx]->load(*src);
        }
      }
      syncMediaWidgets(rState);
    });
  }
}

void Views::unlockTranscopyrightAtCaret() {
  renderer->runWithState([this](RenderState &rState) {
    auto *const caret = rState.caret;
    if (!caret) {
      return;
    }
    const auto docIdx = caret->documentIndex();
    if (docIdx >= session.views().size()) {
      return;
    }
    const auto &openView = session.views()[docIdx];
    const auto holes = session.holesForView(static_cast<std::uint32_t>(docIdx));
    const auto caretPos = caret->byteOffset();
    for (const auto &hole : holes) {
      if (hole.isLocked()) {
        if ((caretPos >= hole.charOffset &&
             caretPos <= hole.charOffset + hole.length) ||
            (caret->hasSelection() &&
             caret->selectionStart() < hole.charOffset + hole.length &&
             caret->selectionEnd() > hole.charOffset)) {
          unlockTranscopyright(openView.storeIndex, hole.span);
          break;
        }
      }
    }
  });
}

void Views::openDocumentPalette() {
  using Field = gleditor::Form::Field;
  using Kind  = gleditor::Form::Kind;

  Field choiceField;
  choiceField.label = "Document";
  choiceField.hint  = "select a document or system xanadoc";
  choiceField.kind  = Kind::Choice;

  for (std::uint8_t k = 0;
       k < static_cast<std::uint8_t>(xudu::SystemDocKind::Count); ++k) {
    const auto kind = static_cast<xudu::SystemDocKind>(k);
    const auto uri  = std::string(xudu::systemDocUri(kind));
    std::string desc;
    switch (kind) {
    case xudu::SystemDocKind::Keymap:
      desc = "Keyboard shortcuts and bindings";
      break;
    case xudu::SystemDocKind::Settings:
      desc = "Typography and editor preferences";
      break;
    case xudu::SystemDocKind::Layout:
      desc = "Multi-column layout and ribbons";
      break;
    case xudu::SystemDocKind::UI:
      desc = "Chrome, status bar, and notifications";
      break;
    case xudu::SystemDocKind::Pouches:
      desc = "Drop zones and persistent span storage";
      break;
    default:
      break;
    }
    std::string option = "⚙ ";
    option += uri;
    option += " — ";
    option += desc;
    choiceField.options.push_back(std::move(option));
    choiceField.optionValues.push_back(uri);
  }

  namespace fs = std::filesystem;
  std::error_code ec;
  const auto curPath = fs::current_path(ec);
  if (!ec) {
    for (const auto &dirEntry : fs::directory_iterator(curPath, ec)) {
      if (dirEntry.is_directory()) {
        const auto &p = dirEntry.path();
        if (fs::exists(p / "ops.nodes") || fs::exists(p / "store.tables") ||
            p.extension() == ".xanadoc") {
          const auto dirName = p.filename().string();
          choiceField.options.push_back("[Local] " + dirName);
          choiceField.optionValues.push_back(p.string());
        }
      }
    }
  }

  choiceField.options.emplace_back("Custom path or file...");
  choiceField.optionValues.emplace_back("__custom__");

  Field customPathField;
  customPathField.label = "Custom path";
  customPathField.hint  = "optional file or store path if custom chosen";

  std::vector<Field> fields;
  fields.push_back(std::move(choiceField));
  fields.push_back(std::move(customPathField));

  form.open("Open Document or System Xanadoc",
            "Select a system xanadoc, local store, or file to open alongside",
            std::move(fields), [this](const std::vector<Field> &answers) {
              std::string chosen           = answers[0].answer();
              const std::string customPath = answers[1].answer();
              if (chosen == "__custom__" || !customPath.empty()) {
                chosen = customPath;
              }
              if (chosen.empty()) {
                return;
              }
              openDocumentFromPath(chosen);
            });
}

void Views::openDocumentFromPath(const std::string &chosen) {
  if (const auto kind = xudu::systemDocKindFromUri(chosen)) {
    const auto sIdx = session.systemStoreIndex(*kind);
    auto &sysStore  = session.store(sIdx);
    const auto head = sysStore.primaryCurrentVersion();
    showAlongside(head, 0.0F, sIdx);
    activateNewest();
    std::cout << "xudu: opened system document " << chosen << " (store " << sIdx
              << ")\n";
    return;
  }

  namespace fs = std::filesystem;
  const fs::path p(chosen);
  if (fs::exists(p / "ops.nodes") || fs::exists(p / "store.tables") ||
      fs::is_directory(p)) {
    try {
      const auto sIdx = session.loadAuxiliaryStore(chosen);
      auto &st        = session.store(sIdx);
      const auto head = st.primaryCurrentVersion();
      showAlongside(head, 0.0F, sIdx);
      activateNewest();
      std::cout << "xudu: opened store " << chosen << " (store " << sIdx
                << ")\n";
    } catch (const std::exception &err) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Could not open xanadoc", err.what());
    }
  } else if (fs::exists(p)) {
    try {
      const auto [sIdx, imported] = session.importFileToTemporaryStore(chosen);
      showAlongside(imported, 0.0F, sIdx);
      activateNewest();
      std::cout << "xudu: imported file " << chosen << " to temporary store "
                << sIdx << "\n";
    } catch (const std::exception &err) {
      state->showDialog(render::DiagnosticSeverity::Error,
                        "Could not import file", err.what());
    }
  } else {
    state->showDialog(render::DiagnosticSeverity::Error, "Path not found",
                      "The specified path does not exist: " + chosen);
  }
}

void Views::selectDoc(const std::uint32_t index) {
  renderer->runWithState([this, index](RenderState &rState) {
    session.flushUncommitted();
    activateDocument(rState, index);
  });
}

void Views::nextDoc() {
  renderer->runWithState([this](RenderState &rState) {
    if (rState.docs.empty()) {
      return;
    }
    const auto total = static_cast<std::uint32_t>(rState.docs.size());
    const auto cur   = switcher ? switcher->activeDocIndex() : 0U;
    const auto next  = (cur + 1U) % total;
    selectDoc(next);
  });
}

void Views::prevDoc() {
  renderer->runWithState([this](RenderState &rState) {
    if (rState.docs.empty()) {
      return;
    }
    const auto total = static_cast<std::uint32_t>(rState.docs.size());
    const auto cur   = switcher ? switcher->activeDocIndex() : 0U;
    const auto prev  = (cur + total - 1U) % total;
    selectDoc(prev);
  });
}

void Views::printHistory() {
  renderer->runWithState([this](RenderState &) {
    const auto here = session.views().empty() ? MicroversionId{}
                                              : session.views().front().version;
    const auto sIdx =
        session.views().empty() ? 0 : session.views().front().storeIndex;
    const auto &st = session.store(sIdx);
    std::cout << "xudu: " << st.opCount() << " operations, "
              << st.primedia().size() << " bytes of primedia\n";
    for (const auto &id : st.allVersions()) {
      const auto op = st.getOp(id);
      std::cout << (id == here ? "  * " : "    ") << id.str() << "  "
                << (op.has_value() ? xudu::opKindName(op->kind) : "?") << "\n";
    }
  });
}

void Views::setOnionSkin(const bool enabled) {
  renderer->runWithState([this, enabled](RenderState &rState) {
    if (onionSkinMode_ == enabled) {
      return;
    }
    onionSkinMode_ = enabled;
    if (onionSkinMode_) {
      auto *const caret = renderer->editCaret();
      if (caret && caret->active() &&
          caret->documentIndex() < rState.docs.size()) {
        activeOnionIdx_ = caret->documentIndex();
      } else if (switcher) {
        activeOnionIdx_ = switcher->activeDocIndex();
      }
      arrangeOnionSkin(rState);
      state->showDialog(render::DiagnosticSeverity::Info, "3D Onion Skin",
                        "Onion skin mode active: scroll wheel cycles " +
                            std::to_string(rState.docs.size()) + " documents.");
    } else {
      arrangeAlongside(rState);
    }
  });
}

void Views::toggleOnionSkin() { setOnionSkin(!onionSkinMode_); }

void Views::arrangeOnionSkin(RenderState &rState) {
  if (rState.docs.empty()) {
    return;
  }
  const auto total  = rState.docs.size();
  const auto active = activeOnionIdx_ % total;
  for (std::size_t i = 0; i < total; ++i) {
    if (!rState.docs[i]) {
      continue;
    }
    const auto k              = (i - active + total) % total;
    const auto kF             = static_cast<float>(k);
    const glm::vec3 targetPos = kF * kDefaultOnionSkinPolicy.offsetPerVersion;
    const float targetOpacity =
        (k == 0) ? 1.0F
                 : std::max(kDefaultOnionSkinPolicy.minimumOpacity,
                            1.0F - kDefaultOnionSkinPolicy.opacityStep * kF);

    auto *const tl = renderer->animTimeline();
    if (tl) {
      rState.docs[i]->animateMoveTo(*tl, targetPos, gleditor::anim::docArrival);
      rState.docs[i]->animateOpacity(*tl, targetOpacity,
                                     gleditor::anim::docArrival);
    } else {
      rState.docs[i]->setModel(glm::translate(glm::mat4(1.0F), targetPos));
      rState.docs[i]->setImmediateOpacity(targetOpacity);
    }
  }
}

void Views::arrangeAlongside(RenderState &rState) {
  for (std::size_t i = 0; i < rState.docs.size(); ++i) {
    if (!rState.docs[i]) {
      continue;
    }
    const auto slot = AbstractRenderer::documentSlot(i);
    auto *const tl  = renderer->animTimeline();
    if (tl) {
      rState.docs[i]->animateMoveTo(*tl, slot, gleditor::anim::docArrival);
      rState.docs[i]->animateOpacity(*tl, 1.0F, gleditor::anim::docArrival);
    } else {
      rState.docs[i]->setModel(glm::translate(glm::mat4(1.0F), slot));
      rState.docs[i]->setImmediateOpacity(1.0F);
    }
  }
}

void Views::cycleOnionSkin(const int delta) {
  renderer->runWithState([this, delta](RenderState &rState) {
    if (rState.docs.empty()) {
      return;
    }
    const int n = static_cast<int>(rState.docs.size());
    int nextIdx = (static_cast<int>(activeOnionIdx_) + delta) % n;
    if (nextIdx < 0) {
      nextIdx += n;
    }
    activeOnionIdx_ = static_cast<std::size_t>(nextIdx);
    arrangeOnionSkin(rState);
    if (switcher) {
      switcher->setActiveDocIndex(static_cast<std::uint32_t>(activeOnionIdx_));
    }
    auto *const caret = renderer->editCaret();
    if (caret) {
      caret->placeAt(static_cast<std::uint32_t>(activeOnionIdx_), 0);
    }
  });
}

} // namespace xudu

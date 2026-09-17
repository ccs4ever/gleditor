/**
 * @file main.cpp
 * @brief The OSMIC client: versioning, transclusion and links.
 *
 * Runs on the library and adds the things that make Xanadu different from an
 * ordinary editor: a history where nothing is deleted, connections drawn
 * between passages, and a map of hypertime.
 *
 * The window and the renderer are the library's; this file is the client
 * logic, the commands and the command-line options.
 */
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.h" // for GLEDITOR_VERSION, TOSTRING
#include <argparse/argparse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/app.hpp>
#include <gleditor/audio.hpp>
#include <gleditor/audio_widget.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/form.hpp>
#include <gleditor/media.hpp>
#include <gleditor/media_stream.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/mimetype.hpp>
#include <gleditor/radial_menu.hpp>
#include <gleditor/render/diagnostics.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text_source.hpp>

#include "xudu/batch_orchestrator.hpp"
#include "xudu/beams.hpp"
#include "xudu/collaborator_overlay.hpp"
#include "xudu/core/config.hpp"
#include "xudu/core/kinetic_tether.hpp"
#include "xudu/core/microversion.hpp"
#include "xudu/core/ops.hpp"
#include "xudu/core/provenance.hpp"
#include "xudu/core/publication.hpp"
#include "xudu/core/publication_ledger.hpp"
#include "xudu/core/resolver.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/swarm_catalog.hpp"
#include "xudu/core/system_docs.hpp"
#include "xudu/core/torrent.hpp"
#include "xudu/core/transcopyright_crypto.hpp"
#include "xudu/core/transcopyright_logic.hpp"
#include "xudu/kinetic_tether_overlay.hpp"
#include "xudu/page_break_overlay.hpp"
#include "xudu/pouch_drawer.hpp"
#include "xudu/satelloid.hpp"
#ifdef XUZZ_BUILD
#include "../zigzag/zigzag_visualizer.hpp"
#include "xudu/bridge_coordinator.hpp"
#endif
#include "xudu/session.hpp"
#include "xudu/swarm_telescope_overlay.hpp"
#include "xudu/tenuous_tether.hpp"
#include "xudu/transcopyright_overlay.hpp"
#include "xudu/wireframe_hull.hpp"

using gleditor::Mod;
using xudu::Author;
using xudu::Config;
using xudu::HoleReason;
using xudu::HypertimeMap;
using xudu::ImageOverlay;
using xudu::KineticTetherEngine;
using xudu::KineticTetherOverlay;
using xudu::Link;
using xudu::LinkBeams;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::PageBreakOverlay;
using xudu::PouchDrawer;
using xudu::PouchItem;
using xudu::PublishedHoleRecord;
using xudu::Scroll;
using xudu::ScrollSegment;
using xudu::SegmentKind;
using xudu::TranscopyrightDescriptor;
using xudu::TranscopyrightLogic;
using xudu::TranscopyrightOverlay;
using xudu::WireframeHullOverlay;
namespace crypto = xudu::crypto;
using xudu::PrimediaSpan;
using xudu::Provenance;
using xudu::PublicationEntry;
using xudu::SatelloidOverlay;
using xudu::Session;
using xudu::SwarmCatalog;
using xudu::SwarmTelescopeOverlay;
using xudu::TenuousTetherOverlay;
using xudu::TetherPayload;

namespace {

/**
 * @brief Z a --background document opens at. LinkBeams::align() fits the camera
 *        to the foreground row (documents with depthZ < 0 are deliberately
 *        excluded from that envelope), so the distance from camera to
 * background is roughly (cameraDistance + |backgroundDepthZ|).
 *
 * -500 is a large enough fraction of a typical camera distance to shrink
 * a background document to roughly half its normal size on screen.
 */
constexpr float backgroundDepthZ = -500.0F;

/// Onion-skin offsets and fading express a historical-context policy: page
/// measurements tell us a page's extent, but cannot choose how much older
/// versions should recede. Keep the fallback explicit until system://layout
/// supplies the live Xudu override.
struct OnionSkinPolicy {
  glm::vec3 offsetPerVersion{18.0F, 14.0F, -10.0F};
  float minimumOpacity{0.20F};
  float opacityStep{0.20F};
};

constexpr OnionSkinPolicy onionSkinPolicy{};

/**
 * @brief Report who signed the authorship record at @p where.
 *
 * A directory, or the record itself; the signature is the file beside it. The
 * two questions are kept apart deliberately: gpg accepting a signature says
 * the record is unaltered since whoever holds that key signed it, and says
 * nothing at all about whether you have any reason to believe that person is
 * who the record claims. Reporting the two as one answer is how a signature
 * becomes a rubber stamp.
 */
int checkAuthorship(const std::string &where) {
  namespace fs = std::filesystem;
  const fs::path given(where);
  const auto record =
      fs::is_directory(given) ? given / xudu::provenanceFileName : given;
  const auto sig = fs::path(record.string() + ".asc");

  const auto slurp = [](const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
  };
  xudu::SignedProvenance sealed{slurp(record), slurp(sig)};
  if (sealed.yaml.empty()) {
    std::cerr << "no authorship record at " << record << "\n";
    return 1;
  }

  const auto check = xudu::verifyProvenance(sealed);
  std::cout << sealed.yaml;
  if (!check.signatureValid) {
    std::cout << "\nxudu: this record is NOT vouched for -- " << check.detail
              << "\n";
    return 1;
  }
  std::cout << "\nxudu: signed by " << check.signer << "\n"
            << "      key " << check.fingerprint << "\n"
            << "      "
            << (check.keyTrusted
                    ? "which is a key this keyring trusts"
                    : "which this keyring has no reason to trust -- the "
                      "signature is real, but that it is this person's key is "
                      "only their say-so")
            << "\n";

  if (const auto said = xudu::parseProvenance(sealed.yaml); said) {
    const auto content = record.parent_path() / xudu::sealedContentName;
    if (const auto bytes = slurp(content); !bytes.empty()) {
      const auto matches = xudu::sha256Hex(bytes) == said->contentDigest &&
                           bytes.size() == said->contentLength;
      std::cout << "      "
                << (matches ? "and it is about the content sealed with it"
                            : "BUT THE CONTENT BESIDE IT IS NOT WHAT IT "
                              "DESCRIBES")
                << "\n";
      if (!matches) {
        return 1;
      }
      // The history, checked the same way and reported separately. A record
      // written before the operations were sealed in says nothing about them,
      // which is not the same as saying they are empty -- so it is reported as
      // not covered rather than passed over in silence.
      const auto ops = record.parent_path() / xudu::sealedOpsName;
      if (said->opsDigest.empty() && 0 == said->opsLength) {
        std::cout << "      but it says nothing about the history sealed "
                     "beside it\n";
        return 0;
      }
      const auto opsBytes  = slurp(ops);
      const auto opsAgrees = xudu::sha256Hex(opsBytes) == said->opsDigest &&
                             opsBytes.size() == said->opsLength;
      std::cout << "      "
                << (opsAgrees
                        ? "and about the history sealed beside it"
                        : "BUT THE HISTORY BESIDE IT IS NOT WHAT IT DESCRIBES")
                << "\n";
      return opsAgrees ? 0 : 1;
    }
  }
  return 0;
}

/**
 * @brief Whether --help-all appears on the command line.
 */
bool wantsEveryOption(const int argc, const char *const *const argv) {
  for (int i = 1; i < argc; i++) {
    if (nullptr != argv[i] && std::string_view{"--help-all"} == argv[i]) {
      return true;
    }
  }
  return false;
}

/**
 * @brief What is on screen, and the commands that change it.
 */
/**
 * @brief Opens documents, and the media widgets and image placements that
 *        come with them.
 *
 * A FrameContributor for one reason only: deviceReady() is the sole place a
 * device and a document pipeline description ever reach anything, and it is
 * only ever called for contributors registered before the render thread
 * starts. MediaWidget instances made later, inside syncMediaWidgets(), are
 * registered from well after that point -- so without capturing both here and
 * handing them on explicitly, a widget's own deviceReady() never runs, its
 * canvas_ stays null, and drawFrame() quietly draws nothing forever. Views
 * itself draws nothing; drawFrame() is a deliberate no-op.
 */
class Views : public gleditor::FrameContributor {
public:
  Views(Session &aSession, RendererRef aRenderer, HypertimeMap &aMap,
        ImageOverlay &aImages, gleditor::Form &aForm, AppStateRef aState,
        std::shared_ptr<gleditor::DocumentSwitcher> aSwitcher)
      : session(aSession), renderer(std::move(aRenderer)), map(aMap),
        images(aImages), form(aForm), state(std::move(aState)),
        switcher(std::move(aSwitcher)) {}

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override {
    device_       = &device;
    documentDesc_ = documentPipeline;
  }

  void drawFrame(gleditor::FrameContext & /*ctx*/) override {}

  /// The audio/video widget attached at @p docOffset within @p doc, as an
  /// anchor a beam can use directly -- see MediaWidget::rectFor(), which
  /// this just forwards to across every widget syncMediaWidgets() has
  /// placed. nullopt when no widget matches, which is any offset that is
  /// plain text, an image (ImageOverlay::rectFor() is the one for those),
  /// or a span whose widget has not been placed yet.
  [[nodiscard]] std::optional<Doc::Anchor>
  widgetRectFor(const Doc &doc, const std::uint32_t docOffset) const {
    for (const auto &widget : mediaWidgets) {
      if (auto rect = widget->rectFor(doc, docOffset)) {
        return rect;
      }
    }
    return std::nullopt;
  }

  /// Replace everything on screen with one document showing @p version.
  void showOnly(const MicroversionId &version,
                const std::size_t storeIndex = 0) {
    const auto count = session.views().size();
    for (std::size_t i = 0; i < count; i++) {
      renderer->push(RenderItemCloseDoc());
    }
    renderer->runWithState([this](RenderState &) { session.clearViews(); });
    showAlongside(version, 0.0F, storeIndex);
  }

  void syncMediaWidgets(RenderState &rState) {
    // Unregister before clear() destroys them: every widget here was handed
    // to the renderer and the picker as a raw pointer, and this function is
    // called again each time another document is opened alongside. Without
    // this, the second call destroys the first call's widgets while their
    // pointers are still live in frameContributors/pickObservers -- which
    // segfaults the next time a frame is drawn, on whichever dangling entry
    // the vector happens to walk into first.
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
      const auto spans = session.mediaSpansFor(vInfo.version, vInfo.storeIndex);
      for (const auto &mSpan : spans) {
        // The whole file this span was classified against, not just the
        // (possibly narrower) span itself: a fragment transcluded out of the
        // middle of a media file carries no header of its own, so a decoder
        // needs the container's bytes to make sense of any of it. Reading
        // this unconditionally costs nothing extra for the common case,
        // where the span already covers the whole file (containerOffset is
        // 0 and containerLength equals the span's own length, so this is the
        // same span read()'s always been given).
        const auto bytes = st.read(xudu::PrimediaSpan{
            mSpan.span.scroll, mSpan.span.start - mSpan.containerOffset,
            mSpan.containerLength});
        if (mSpan.isImage) {
          // A picture has no play, pause or seek: it goes to the image
          // overlay's shared pipeline rather than a MediaWidget, whose whole
          // UI is built around a MediaPlayer this span does not have.
          //
          // Keyed by the *container's* coordinates rather than this span's
          // own, so that two different fragments transcluded out of the same
          // image -- which now both decode and show that whole image, see
          // above -- share one cache entry instead of decoding it twice.
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
        // addFrameContributor() below does not call deviceReady() -- that
        // only happens once, for whatever is already registered when the
        // render thread starts, which this widget is not. Without this call
        // its canvas_ stays null and drawFrame() never draws anything.
        if (nullptr != device_) {
          widget->deviceReady(*device_, documentDesc_);
        }
        auto stream = std::make_shared<gleditor::MemoryMediaStream>(bytes);
        // loadFragment() with a fragment covering the whole container (the
        // common case) behaves exactly like load(); only a span narrower
        // than its container -- a temporal transclusion -- ends up deferring
        // a setTimeRange() call until playback reports a real duration.
        widget->loadFragment(
            gleditor::MediaResource::fromStream(stream, mSpan.label),
            gleditor::ByteRange{mSpan.containerOffset, mSpan.span.length},
            mSpan.containerLength);
        widget->setTitle(mSpan.label);
        widget->attachToDocument(rState.docs[dIdx], mSpan.docOffset);
        // mSpan.widgetWidth/widgetHeight is exactly what
        // Session::placeholderFor() reserved for this span (see
        // mediaSpansFor() in apps/xudu/session.cpp, which computes both from
        // the one shared formula) -- read from there rather than
        // recomputed here, so this widget's size and the room set aside for
        // it in the text flow cannot drift into disagreeing.
        widget->setSize(mSpan.widgetWidth, mSpan.widgetHeight);
        widget->setVisible(true);
        renderer->addFrameContributor(widget.get());
        renderer->addPickObserver(widget.get());
        state->accessibility->addSource(widget.get());
        mediaWidgets.push_back(std::move(widget));
      }
    }
  }

  /// Open @p version as another document beside whatever is already there.
  void showAlongside(const MicroversionId &version, const float depthZ = 0.0F,
                     const std::size_t storeIndex = 0) {
    renderer->push(
        RenderItemOpenDoc(session.sourceFor(version, storeIndex), depthZ));
    renderer->runWithState([this, version, storeIndex](RenderState &rState) {
      if (rState.docs.empty()) {
        return;
      }
      primaryDocument_ = rState.docs.front();
      rState.docs.back()->addObserver(&session);
      session.viewOpened(version, storeIndex);
      map.setCurrent(session.views().front().version);
      syncMediaWidgets(rState);
    });
  }

  [[nodiscard]] std::optional<glm::mat4> presentationTransform() const {
    const auto document = primaryDocument_.lock();
    if (!document) {
      return std::nullopt;
    }
    const auto frame = document->pageFrame(0);
    if (!frame) {
      return std::nullopt;
    }
    // The document's measured margin is the smallest readable gap for an
    // adjacent presentation. It follows custom page geometry and reflow
    // instead of inventing a second, fixed Xuzz gutter.
    return frame->localToWorld *
           glm::translate(
               glm::mat4{1.0F},
               glm::vec3{frame->rightPx + frame->marginPx, 0.0F, 0.0F});
  }

  /// Where the caret is, and what it has selected, on the render thread.
  struct Where {
    std::uint32_t doc{};
    std::uint32_t start{};
    std::uint32_t end{};
    bool hasRange{};
  };

  template <typename Fun> void withCaret(Fun fun) {
    renderer->runWithState([this, fun](RenderState &rState) {
      auto *const caret = renderer->editCaret();
      if (nullptr == caret || !caret->active() ||
          caret->documentIndex() >= rState.docs.size()) {
        return;
      }
      session.flushUncommitted(caret->documentIndex());
      Where where{caret->documentIndex(), caret->byteOffset(),
                  caret->byteOffset(), caret->hasSelection()};
      if (where.hasRange) {
        where.start = caret->selectionStart();
        where.end   = caret->selectionEnd();
      }
      fun(rState, where, caret);
    });
  }

  void swingBackToSpan(const PouchItem &item) {
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

  void back() {
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

  void forward() {
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

  void scrubHistory(const bool backward) {
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

  void deleteSelection() {
    withCaret([](RenderState &rState, const Where &where, Caret *caret) {
      if (!where.hasRange) {
        return;
      }
      rState.docs[where.doc]->erase(rState, where.start,
                                    where.end - where.start, caret);
    });
  }

  void transcludeSelection() {
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

  void linkSelection() {
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
        pending = Pending{where.doc, where.start, where.end, std::move(spans)};
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

  void cancelLink() {
    renderer->runWithState([this](RenderState &) {
      if (!pending) {
        std::cout << "xudu: no link waiting for its other end\n";
        return;
      }
      std::cout << "xudu: dropped the link begun at doc " << pending->doc
                << " [" << pending->start << "," << pending->end << ")\n";
      pending.reset();
    });
  }

  void publishCurrent(const std::string &salt) {
    renderer->runWithState([this, salt](RenderState &) {
      if (session.views().empty()) {
        std::cout << "xudu: nothing open to publish\n";
        state->showDialog(render::DiagnosticSeverity::Warning,
                          "Nothing to publish",
                          "No document is open. Open one, and what is under "
                          "the caret is what gets published.");
        return;
      }
      auto *const caret = renderer->editCaret();
      const auto which = nullptr != caret && caret->active() &&
                                 caret->documentIndex() < session.views().size()
                             ? caret->documentIndex()
                             : 0U;
      const auto version  = session.versionOf(which);
      const auto storeIdx = session.storeIndexOf(which);
      const auto who      = session.author();
      const auto where    = session.publishedDir(storeIdx);

      using Field = gleditor::Form::Field;
      using Kind  = gleditor::Form::Kind;

      Field keys{"Signing key",
                 {},
                 "no signing key in the keyring",
                 false,
                 Kind::Choice};
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

      std::vector<Field> asked{
          Field{"Name", salt.empty() ? std::string{"document"} : salt,
                "one word; publishing again under it is a further state of "
                "this document",
                true},
          Field{"Title", {}, "what this document is called", true},
          Field{"Author", who.name, "who is publishing this", true},
          Field{"Email", who.email, "how to reach them", true},
          std::move(keys),
          Field{"Passphrase",
                {},
                "only if the agent is not holding it",
                false,
                Kind::Secret},
          [] {
            Field toggle{"", {}, {}, false, Kind::Toggle};
            toggle.revealsSecrets = true;
            return toggle;
          }(),
          Field{"Rights", {}, "how others may use this; optional", false},
          Field{"Note", {}, "anything else worth recording; optional", false},
      };
      asked[1].value = asked[0].value;

      form.open(
          "Publish " + version.str(),
          "Signed as an authorship record, then sealed into " + where,
          std::move(asked),
          [this, version, which, storeIdx](const std::vector<Field> &answers) {
            publishAnswers(version, which, storeIdx, answers);
          });
    });
  }

  void publishAnswers(const MicroversionId &version, const std::uint32_t which,
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

  /// Save or preserve current document
  void saveCurrent() {
    renderer->runWithState([this](RenderState &) {
      if (session.views().empty()) {
        session.saveAll();
        return;
      }
      auto *const caret = renderer->editCaret();
      const auto which = nullptr != caret && caret->active() &&
                                 caret->documentIndex() < session.views().size()
                             ? caret->documentIndex()
                             : (switcher ? switcher->activeDocIndex() : 0U);
      if (which >= session.views().size()) {
        session.saveAll();
        return;
      }
      const auto storeIdx = session.storeIndexOf(which);
      if (session.isTemporaryStore(storeIdx)) {
        // Open folder dialog to preserve temporary store
        using Field         = gleditor::Form::Field;
        namespace fs        = std::filesystem;
        std::string curDir  = fs::current_path().string();
        std::string defName = "doc_" + std::to_string(storeIdx) + ".xanadoc";

        std::vector<Field> fields{
            Field{"Folder", curDir,
                  "directory where the xanadoc folder will live", true},
            Field{"Name", defName, "name of the xanadoc folder", true},
        };

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

  void preserveAnswers(const std::size_t storeIdx,
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

  void closeDocument(const std::uint32_t docIndex) {
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

  void closeActive() {
    renderer->runWithState([this](RenderState &rState) {
      if (rState.docs.empty()) {
        return;
      }
      auto *const caret = renderer->editCaret();
      const auto which  = nullptr != caret && caret->active() &&
                                  caret->documentIndex() < rState.docs.size()
                              ? caret->documentIndex()
                              : (switcher ? switcher->activeDocIndex() : 0U);
      if (which < rState.docs.size()) {
        closeDocument(which);
      }
    });
  }

  void newDocument() {
    const auto storeIndex = session.createNewStore("");
    showAlongside(MicroversionId{}, 0.0F, storeIndex);
    renderer->runWithState([this](RenderState &rState) {
      if (!rState.docs.empty()) {
        const auto newDocIndex =
            static_cast<std::uint32_t>(rState.docs.size() - 1);
        if (switcher) {
          switcher->setActiveDocIndex(newDocIndex);
        }
        auto *const caret = renderer->editCaret();
        if (caret) {
          caret->placeAt(newDocIndex, 0);
        }
      }
    });
    std::cout << "xudu: created new sovereign document (store " << storeIndex
              << ")\n";
  }

  void spawnTranscludedDocument(const TetherPayload &payload,
                                const float /*screenX*/ = 0.0F,
                                const float /*screenY*/ = 0.0F) {
    if (payload.originCharEnd <= payload.originCharStart) {
      return;
    }
    const auto len = payload.originCharEnd - payload.originCharStart;
    const auto spawnedVer =
        session.store(0).transclude(MicroversionId{}, 0, payload.originVersion,
                                    payload.originCharStart, len);
    showAlongside(spawnedVer, 0.0F, 0);
    renderer->runWithState([this](RenderState &rState) {
      if (!rState.docs.empty()) {
        const auto newDocIndex =
            static_cast<std::uint32_t>(rState.docs.size() - 1);
        if (switcher) {
          switcher->setActiveDocIndex(newDocIndex);
        }
        auto *const caret = renderer->editCaret();
        if (caret) {
          caret->placeAt(newDocIndex, 0);
        }
      }
    });
    std::cout << "xudu: spawned transcluded document version "
              << spawnedVer.str() << " from origin version "
              << payload.originVersion.str() << " [" << payload.originCharStart
              << ", " << payload.originCharEnd << ")\n";
  }

  void summonPublication(const PublicationEntry &entry) {
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
    renderer->runWithState([this](RenderState &rState) {
      if (!rState.docs.empty()) {
        const auto newDocIndex =
            static_cast<std::uint32_t>(rState.docs.size() - 1);
        if (switcher) {
          switcher->setActiveDocIndex(newDocIndex);
        }
        auto *const caret = renderer->editCaret();
        if (caret) {
          caret->placeAt(newDocIndex, 0);
        }
      }
    });
    std::cout << "xudu: summoned publication '" << entry.title
              << "' into 3D space (store " << storeIndex << ")\n";
  }

  void insertPageBreak(const std::uint32_t docIndex,
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

  void insertPageBreakAtCaret() {
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
      std::cout << "xudu: page break inserted at doc " << where.doc
                << " offset " << where.start << "\n";
    });
  }

  void exportOsmic() {
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

  void importFile(const std::string &filePath) {
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

  void setWireframeOverlay(WireframeHullOverlay *overlay) noexcept {
    wireframeOverlay_ = overlay;
  }

  void unlockTranscopyright(const std::size_t storeIdx,
                            const PrimediaSpan &span) {
    if (session.unlockTranscopyright(storeIdx, span)) {
      renderer->runWithState([this, storeIdx](RenderState &rState) {
        for (std::size_t dIdx = 0;
             dIdx < rState.docs.size() && dIdx < session.views().size();
             ++dIdx) {
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

  void unlockTranscopyrightAtCaret() {
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
      const auto holes =
          session.holesForView(static_cast<std::uint32_t>(docIdx));
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

  void openDocumentPalette() {
    using Field = gleditor::Form::Field;
    using Kind  = gleditor::Form::Kind;

    Field choiceField;
    choiceField.label = "Document";
    choiceField.hint  = "select a document or system xanadoc";
    choiceField.kind  = Kind::Choice;

    // 1. Add all system xanadocs
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
      choiceField.options.push_back("⚙ " + uri + " — " + desc);
      choiceField.optionValues.push_back(uri);
    }

    // 2. Discover local xanadoc directories in current path
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto curPath = fs::current_path(ec);
    if (!ec) {
      for (const auto &dirEntry : fs::directory_iterator(curPath, ec)) {
        if (dirEntry.is_directory()) {
          const auto p = dirEntry.path();
          if (fs::exists(p / "ops.nodes") || fs::exists(p / "store.tables") ||
              p.extension() == ".xanadoc") {
            const auto dirName = p.filename().string();
            choiceField.options.push_back("[Local] " + dirName);
            choiceField.optionValues.push_back(p.string());
          }
        }
      }
    }

    choiceField.options.push_back("Custom path or file...");
    choiceField.optionValues.push_back("__custom__");

    Field customPathField;
    customPathField.label = "Custom path";
    customPathField.hint  = "optional file or store path if custom chosen";

    std::vector<Field> fields{
        std::move(choiceField),
        std::move(customPathField),
    };

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

  void openDocumentFromPath(const std::string &chosen) {
    if (const auto kind = xudu::systemDocKindFromUri(chosen)) {
      const auto sIdx = session.systemStoreIndex(*kind);
      auto &sysStore  = session.store(sIdx);
      const auto head = sysStore.primaryCurrentVersion();
      showAlongside(head, 0.0F, sIdx);
      renderer->runWithState([this](RenderState &rState) {
        if (!rState.docs.empty()) {
          const auto newDocIndex =
              static_cast<std::uint32_t>(rState.docs.size() - 1);
          if (switcher) {
            switcher->setActiveDocIndex(newDocIndex);
          }
          auto *const caret = renderer->editCaret();
          if (caret) {
            caret->placeAt(newDocIndex, 0);
          }
        }
      });
      std::cout << "xudu: opened system document " << chosen << " (store "
                << sIdx << ")\n";
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
        renderer->runWithState([this](RenderState &rState) {
          if (!rState.docs.empty()) {
            const auto newDocIndex =
                static_cast<std::uint32_t>(rState.docs.size() - 1);
            if (switcher) {
              switcher->setActiveDocIndex(newDocIndex);
            }
            auto *const caret = renderer->editCaret();
            if (caret) {
              caret->placeAt(newDocIndex, 0);
            }
          }
        });
        std::cout << "xudu: opened store " << chosen << " (store " << sIdx
                  << ")\n";
      } catch (const std::exception &err) {
        state->showDialog(render::DiagnosticSeverity::Error,
                          "Could not open xanadoc", err.what());
      }
    } else if (fs::exists(p)) {
      try {
        const auto [sIdx, imported] =
            session.importFileToTemporaryStore(chosen);
        showAlongside(imported, 0.0F, sIdx);
        renderer->runWithState([this](RenderState &rState) {
          if (!rState.docs.empty()) {
            const auto newDocIndex =
                static_cast<std::uint32_t>(rState.docs.size() - 1);
            if (switcher) {
              switcher->setActiveDocIndex(newDocIndex);
            }
            auto *const caret = renderer->editCaret();
            if (caret) {
              caret->placeAt(newDocIndex, 0);
            }
          }
        });
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

  void selectDoc(const std::uint32_t index) {
    renderer->runWithState([this, index](RenderState &rState) {
      session.flushUncommitted();
      if (index < rState.docs.size() && rState.docs[index]) {
        if (switcher) {
          switcher->setActiveDocIndex(index);
        }
        auto *const caret = renderer->editCaret();
        if (caret) {
          caret->placeAt(index, 0);
        }
      }
    });
  }

  void nextDoc() {
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

  void prevDoc() {
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

  void printHistory() {
    renderer->runWithState([this](RenderState &) {
      const auto here = session.views().empty()
                            ? MicroversionId{}
                            : session.views().front().version;
      const auto sIdx =
          session.views().empty() ? 0 : session.views().front().storeIndex;
      const auto &st = session.store(sIdx);
      std::cout << "xudu: " << st.opCount() << " operations, "
                << st.primedia().size() << " bytes of primedia\n";
      for (const auto &id : st.allVersions()) {
        const auto op = st.getOp(id);
        std::cout << (id == here ? "  * " : "    ") << id.str() << "  "
                  << (op.has_value() ? xudu::opKindName(op->kind) : "?")
                  << "\n";
      }
    });
  }

  [[nodiscard]] bool onionSkinMode() const noexcept { return onionSkinMode_; }

  void setOnionSkin(const bool enabled) {
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
                              std::to_string(rState.docs.size()) +
                              " documents.");
      } else {
        arrangeAlongside(rState);
      }
    });
  }

  void toggleOnionSkin() { setOnionSkin(!onionSkinMode_); }

  void arrangeOnionSkin(RenderState &rState) {
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
      const float kF            = static_cast<float>(k);
      const glm::vec3 targetPos = kF * onionSkinPolicy.offsetPerVersion;
      const float targetOpacity =
          (k == 0) ? 1.0F
                   : std::max(onionSkinPolicy.minimumOpacity,
                              1.0F - onionSkinPolicy.opacityStep * kF);

      auto *const tl = renderer->animTimeline();
      if (tl) {
        rState.docs[i]->animateMoveTo(*tl, targetPos,
                                      gleditor::anim::docArrival);
        rState.docs[i]->animateOpacity(*tl, targetOpacity,
                                       gleditor::anim::docArrival);
      } else {
        rState.docs[i]->setModel(glm::translate(glm::mat4(1.0F), targetPos));
        rState.docs[i]->setImmediateOpacity(targetOpacity);
      }
    }
  }

  void arrangeAlongside(RenderState &rState) {
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

  void cycleOnionSkin(const int delta) {
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
        switcher->setActiveDocIndex(
            static_cast<std::uint32_t>(activeOnionIdx_));
      }
      auto *const caret = renderer->editCaret();
      if (caret) {
        caret->placeAt(static_cast<std::uint32_t>(activeOnionIdx_), 0);
      }
    });
  }

private:
  struct Pending {
    std::uint32_t doc{};
    std::uint32_t start{};
    std::uint32_t end{};
    std::vector<xudu::PrimediaSpan> spans;
  };

  Session &session;
  RendererRef renderer;
  HypertimeMap &map;
  ImageOverlay &images;
  gleditor::Form &form;
  AppStateRef state;
  std::shared_ptr<gleditor::DocumentSwitcher> switcher;
  std::weak_ptr<Doc> primaryDocument_;
  std::optional<Pending> pending;
  std::vector<std::shared_ptr<gleditor::MediaWidget>> mediaWidgets;
  bool onionSkinMode_{false};
  std::size_t activeOnionIdx_{0};

  /// Set in deviceReady(), so a MediaWidget made later in syncMediaWidgets()
  /// can be handed the same device and pipeline description explicitly.
  render::RenderDevice *device_{nullptr};
  render::PipelineDesc documentDesc_;
  WireframeHullOverlay *wireframeOverlay_{nullptr};
};

void bindCommands(gleditor::Application &app, const AppStateRef &state,
                  Views &views, HypertimeMap &map, LinkBeams &links,
                  Session &session,
                  const std::shared_ptr<gleditor::RadialMenu> &radialMenu,
                  const RendererRef &renderer, PouchDrawer &pouchDrawer,
                  SwarmTelescopeOverlay &swarmTelescope,
                  const std::string &publishAs) {
  app.bindDefaultViewCommands();

  app.commands().bind(SDL_SCANCODE_Q, Mod::Ctrl, "quit", "save and close",
                      [state, &session] {
                        session.saveAll();
                        state->alive = false;
                      });
  app.commands().bind(SDL_SCANCODE_S, Mod::Ctrl, "save",
                      "save or preserve active document",
                      [&views] { views.saveCurrent(); });
  app.commands().bind(SDL_SCANCODE_W, Mod::Ctrl, "close",
                      "close the active document",
                      [&views] { views.closeActive(); });
  app.commands().bind(SDL_SCANCODE_TAB, Mod::Ctrl, "next-doc",
                      "switch to next document", [&views] { views.nextDoc(); });
  app.commands().bind(SDL_SCANCODE_TAB, Mod::Ctrl | Mod::Shift, "prev-doc",
                      "switch to previous document",
                      [&views] { views.prevDoc(); });

  for (int i = 1; i <= 9; ++i) {
    const auto scancode = static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (i - 1));
    const auto targetIndex = static_cast<std::uint32_t>(i - 1);
    app.commands().bind(
        scancode, Mod::Ctrl, "doc-" + std::to_string(i),
        "switch to document " + std::to_string(i),
        [&views, targetIndex] { views.selectDoc(targetIndex); });
  }

  app.commands().bind(SDL_SCANCODE_H, Mod::Ctrl, "map",
                      "show or hide the hypertime map",
                      [&map] { map.toggle(); });
  app.commands().bind(SDL_SCANCODE_M, Mod::Ctrl, "radial-menu",
                      "open radial menu for formatting and alignment",
                      [state, radialMenu, renderer] {
                        renderer->runWithState([state, radialMenu,
                                                renderer](RenderState &rState) {
                          if (radialMenu->isOpen()) {
                            radialMenu->close();
                            return;
                          }
                          auto *const caret   = renderer->editCaret();
                          std::uint32_t doc   = 0;
                          std::uint32_t start = 0;
                          std::uint32_t len   = 0;
                          if (caret && caret->active() &&
                              caret->documentIndex() < rState.docs.size()) {
                            doc = caret->documentIndex();
                            if (caret->hasSelection()) {
                              start = caret->selectionStart();
                              len   = caret->selectionEnd() - start;
                            } else {
                              start = caret->byteOffset();
                            }
                          }
                          float mx = static_cast<float>(state->mouseX);
                          float my = static_cast<float>(state->mouseY);
                          if (mx <= 0.0F && my <= 0.0F && state->clickX >= 0) {
                            mx = static_cast<float>(state->clickX);
                            my = static_cast<float>(state->clickY);
                          }
                          radialMenu->openAtWindowCoords(mx, my, doc, start,
                                                         len);
                        });
                      });
  app.commands().bind(SDL_SCANCODE_B, Mod::Ctrl, "back",
                      "go to the previous state, losing nothing",
                      [&views] { views.back(); });
  app.commands().bind(SDL_SCANCODE_N, Mod::Ctrl, "new-doc",
                      "create a new sovereign document quad",
                      [&views] { views.newDocument(); });
  app.commands().bind(SDL_SCANCODE_N, Mod::Ctrl | Mod::Shift, "forward",
                      "go to the next state in hypertime",
                      [&views] { views.forward(); });
  app.commands().bind(SDL_SCANCODE_O, Mod::Ctrl, "open-doc",
                      "open a document or system xanadoc",
                      [&views] { views.openDocumentPalette(); });
  app.commands().bind(SDL_SCANCODE_O, Mod::Ctrl | Mod::Shift, "onion-skin",
                      "toggle 3D multi-document onion skinning mode",
                      [&views] { views.toggleOnionSkin(); });
  app.commands().bind(SDL_SCANCODE_BACKSLASH, Mod::Ctrl, "pouch-toggle",
                      "toggle screen-edge pouch drawer and clasp bench",
                      [&pouchDrawer] { pouchDrawer.toggle(); });
  app.commands().bind(SDL_SCANCODE_F2, Mod::None, "pouch-toggle-f2",
                      "toggle screen-edge pouch drawer and clasp bench",
                      [&pouchDrawer] { pouchDrawer.toggle(); });
  app.commands().bind(SDL_SCANCODE_T, Mod::Ctrl | Mod::Shift,
                      "telescope-toggle",
                      "toggle decentralized swarm telescope overlay",
                      [&swarmTelescope] { swarmTelescope.toggle(); });
  app.commands().bind(SDL_SCANCODE_F3, Mod::None, "telescope-toggle-f3",
                      "toggle decentralized swarm telescope overlay",
                      [&swarmTelescope] { swarmTelescope.toggle(); });
  app.commands().bind(SDL_SCANCODE_F4, Mod::None, "tension-physics-toggle",
                      "toggle 3-way tension spring layout simulation",
                      [&links] { links.togglePhysics(); });
  app.commands().bind(SDL_SCANCODE_F5, Mod::None, "unlock-transcopyright-f5",
                      "unlock transcopyright span at caret or selection",
                      [&views] { views.unlockTranscopyrightAtCaret(); });
  app.commands().bind(SDL_SCANCODE_U, Mod::Ctrl, "unlock-transcopyright-ctrl-u",
                      "unlock transcopyright span at caret or selection",
                      [&views] { views.unlockTranscopyrightAtCaret(); });
  app.commands().bind(SDL_SCANCODE_LEFTBRACKET, Mod::Ctrl, "scrub-back",
                      "scrub backward in hypertime history",
                      [&views] { views.scrubHistory(true); });
  app.commands().bind(SDL_SCANCODE_RIGHTBRACKET, Mod::Ctrl, "scrub-forward",
                      "scrub forward in hypertime history",
                      [&views] { views.scrubHistory(false); });
  app.commands().bind(SDL_SCANCODE_T, Mod::Ctrl, "transclude",
                      "transclude the selection into a second document",
                      [&views] { views.transcludeSelection(); });
  app.commands().bind(SDL_SCANCODE_L, Mod::Ctrl, "xanalink",
                      "mark one end of a xanalink, then join it to another "
                      "selection -- in this document or any other open one",
                      [&views] { views.linkSelection(); });
  app.commands().bind(SDL_SCANCODE_L, Mod::Ctrl | Mod::Shift, "cancel link",
                      "forget a xanalink that was begun and not finished",
                      [&views] { views.cancelLink(); });
  app.commands().bind(
      SDL_SCANCODE_K, Mod::Ctrl, "beams",
      "show or hide the links and transclusions between documents",
      [&links] { links.toggle(); });
  app.commands().bind(SDL_SCANCODE_K, Mod::Ctrl | Mod::Shift, "sworph",
                      "let a link coming into view bring its far document over",
                      [&links] { links.setSworph(!links.sworphing()); });
  app.commands().bind(SDL_SCANCODE_S, Mod::Ctrl | Mod::Shift, "publish",
                      "publish the document the caret is in, so it can be read "
                      "off this machine",
                      [&views, publishAs] { views.publishCurrent(publishAs); });
  app.commands().bind(SDL_SCANCODE_P, Mod::Ctrl, "history",
                      "print every state to the terminal",
                      [&views] { views.printHistory(); });
  app.commands().bind(SDL_SCANCODE_BACKSPACE, "delete",
                      "stop pointing at the selection",
                      [&views] { views.deleteSelection(); });
  app.commands().bind(SDL_SCANCODE_RETURN, Mod::Ctrl, "page-break",
                      "insert a page break at the caret position",
                      [&views] { views.insertPageBreakAtCaret(); });

  const auto applyDecoration = [&session,
                                renderer](const gleditor::Decoration deco) {
    renderer->runWithState([&session, renderer, deco](RenderState &rState) {
      auto *const caret = renderer->editCaret();
      if (caret && caret->active() &&
          caret->documentIndex() < rState.docs.size() &&
          caret->hasSelection()) {
        const auto doc   = caret->documentIndex();
        const auto start = caret->selectionStart();
        const auto len   = caret->selectionEnd() - start;
        session.markDecorated(doc, start, len, gleditor::decorationBit(deco));
      }
    });
  };

  app.commands().bind(
      0, "format-bold", "toggle bold on selected text",
      [applyDecoration] { applyDecoration(gleditor::Decoration::Bold); });
  app.commands().bind(
      0, "format-italic", "toggle italic on selected text",
      [applyDecoration] { applyDecoration(gleditor::Decoration::Italic); });
  app.commands().bind(
      0, "format-underline", "toggle underline on selected text",
      [applyDecoration] { applyDecoration(gleditor::Decoration::Underline); });
  app.commands().bind(0, "format-strikethrough",
                      "toggle strikethrough on selected text",
                      [applyDecoration] {
                        applyDecoration(gleditor::Decoration::Strikethrough);
                      });
  app.commands().bind(0, "format-superscript",
                      "toggle superscript on selected text", [applyDecoration] {
                        applyDecoration(gleditor::Decoration::Superscript);
                      });
  app.commands().bind(
      0, "format-subscript", "toggle subscript on selected text",
      [applyDecoration] { applyDecoration(gleditor::Decoration::Subscript); });

  app.commands().bind(0, "save-document", "save or preserve active document",
                      [&views] { views.saveCurrent(); });
  app.commands().bind(0, "export-osmic",
                      "export OSMIC text spool representation",
                      [&views] { views.exportOsmic(); });
  app.commands().bind(0, "insert-break",
                      "insert a page break at the caret position",
                      [&views] { views.insertPageBreakAtCaret(); });

  const auto dropSelectionToBench = [&views, &pouchDrawer,
                                     &session](const bool isLeft) {
    views.withCaret([&pouchDrawer, &session, isLeft](
                        RenderState &, const Views::Where &where, Caret *) {
      if (!where.hasRange) {
        std::cout << "xudu: select text to drop onto bench first\n";
        return;
      }
      const auto docIdx   = where.doc;
      const auto storeIdx = session.storeIndexOf(docIdx);
      const auto ver      = session.versionOf(docIdx);
      const auto spans    = session.store(storeIdx).rebuild(ver).spansFor(
          where.start, where.end - where.start);
      if (spans.empty()) {
        return;
      }
      const auto text    = session.store(storeIdx).textOf(ver);
      const auto preview = text.substr(
          where.start, std::min<std::size_t>(where.end - where.start, 64));
      PouchItem item;
      item.itemId          = 0;
      item.span            = spans.front();
      item.previewText     = preview;
      item.originVersion   = ver;
      item.originDocIndex  = docIdx;
      item.originCharStart = where.start;
      item.originCharEnd   = where.end;
      if (isLeft) {
        pouchDrawer.forge().dropLeft(std::move(item));
        std::cout << "xudu: dropped span onto clasp left bench: '" << preview
                  << "'\n";
      } else {
        pouchDrawer.forge().dropRight(std::move(item));
        std::cout << "xudu: dropped span onto clasp right bench: '" << preview
                  << "'\n";
      }
    });
  };

  const auto dropSelectionToZone = [&views, &pouchDrawer,
                                    &session](const std::string_view zoneId) {
    views.withCaret([&pouchDrawer, &session, zoneId](
                        RenderState &, const Views::Where &where, Caret *) {
      if (!where.hasRange) {
        std::cout << "xudu: select text to drop into pouch first\n";
        return;
      }
      const auto docIdx   = where.doc;
      const auto storeIdx = session.storeIndexOf(docIdx);
      const auto ver      = session.versionOf(docIdx);
      const auto spans    = session.store(storeIdx).rebuild(ver).spansFor(
          where.start, where.end - where.start);
      if (spans.empty()) {
        return;
      }
      const auto text    = session.store(storeIdx).textOf(ver);
      const auto preview = text.substr(
          where.start, std::min<std::size_t>(where.end - where.start, 64));
      const auto item = pouchDrawer.manager().dropSpan(
          zoneId, spans.front(), preview, ver, docIdx, where.start, where.end);
      std::cout << "xudu: dropped span into pouch zone '" << zoneId
                << "' (item " << item.itemId << ")\n";
    });
  };

  app.commands().bind(0, "pouch-drop-left",
                      "drop selection onto clasp homestead bench (left)",
                      [dropSelectionToBench] { dropSelectionToBench(true); });
  app.commands().bind(0, "pouch-drop-right",
                      "drop selection onto clasp toward bench (right)",
                      [dropSelectionToBench] { dropSelectionToBench(false); });
  app.commands().bind(0, "pouch-drop", "drop selection into active pouch zone",
                      [dropSelectionToZone] { dropSelectionToZone("notes"); });
  app.commands().bind(0, "pouch-drop-notes",
                      "drop selection into notes pouch zone",
                      [dropSelectionToZone] { dropSelectionToZone("notes"); });
  app.commands().bind(
      0, "pouch-drop-scratch", "drop selection into scratch pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("scratch"); });
  app.commands().bind(
      0, "pouch-drop-to-link-left",
      "drop selection into to-link-left pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("to_link_left"); });
  app.commands().bind(
      0, "pouch-drop-to-link-right",
      "drop selection into to-link-right pouch zone",
      [dropSelectionToZone] { dropSelectionToZone("to_link_right"); });

  app.commands().bind(
      0, "forge-clasp", "forge bilateral clasp link from items on bench",
      [&pouchDrawer, &session, renderer] {
        renderer->runWithState([&pouchDrawer, &session,
                                renderer](RenderState &) {
          if (!pouchDrawer.forge().canForge()) {
            std::cout << "xudu: clasp forge requires items on "
                         "both left and right benches\n";
            return;
          }
          auto *const caret = renderer->editCaret();
          const auto docIdx = (nullptr != caret && caret->active() &&
                               caret->documentIndex() < session.views().size())
                                  ? caret->documentIndex()
                                  : 0U;
          if (pouchDrawer.forge().forge(session, docIdx)) {
            std::cout << "xudu: forged clasp link on active "
                         "document "
                      << docIdx << "\n";
          }
        });
      });

  app.commands().bind(0, "clear-bench", "clear items from clasp forge bench",
                      [&pouchDrawer] {
                        pouchDrawer.forge().clearLeft();
                        pouchDrawer.forge().clearRight();
                        std::cout << "xudu: cleared clasp forge bench\n";
                      });
}

} // namespace

int main(const int argc, char **argv) {
  gleditor::initLocale();

  const auto state    = std::make_shared<AppState>();
  const bool detailed = wantsEveryOption(argc, argv);

  argparse::ArgumentParser parser("xudu", TOSTRING(GLEDITOR_VERSION));
  gleditor::addCommonArguments(parser, detailed);
  parser.add_argument("store")
      .help("directory the primary spools live in; created if it is not there")
      .default_value(std::string{"xanadoc"});
  parser.add_argument("--version-id")
      .help("microversion to open, for example 2a4; the default is the most "
            "recent state in the store")
      .default_value(std::string{});
  parser.add_argument("--alongside")
      .help(
          "a second microversion to show beside the opening one, for instance "
          "to compare two states")
      .default_value(std::string{});
  parser.add_argument("--background")
      .help("open this microversion as a background document (depthZ < 0), "
            "standing behind the foreground row and excluded from camera "
            "auto-framing; repeatable")
      .append();
  parser.add_argument("--no-beams")
      .help("do not draw the connections between documents")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--no-sworph")
      .help("do not let a link coming into view bring its far document over")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--map")
      .help("show the hypertime map on startup; ctrl-h toggles it while "
            "running")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--compare")
      .help("compare microversions in hypertime map diff panel, e.g. "
            "v1,v2,v3; repeatable")
      .append();
  parser.add_argument("--alias")
      .help("assign alias to microversion as VERSION:ALIAS; repeatable")
      .append();
  parser.add_argument("--onion-skin")
      .help("visualize open documents stacked in 3D depth with opacity decay; "
            "scroll wheel cycles")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--pouch")
      .help("open the screen-edge pouch drawer on startup; ctrl-\\ or F2 "
            "toggles it")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--telescope")
      .help(
          "open decentralized swarm telescope overlay on startup; ctrl-shift-T "
          "or F3 toggles it")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--physics")
      .help("enable 3-way tension spring layout simulation for document "
            "positioning")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--tension-layout")
      .help("alias for --physics")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("--audio")
      .help(
          "open an audio stream or file as an embedded AudioWidget; repeatable")
      .append();
  parser.add_argument("--video")
      .help(
          "open a video stream or file as an embedded MediaWidget; repeatable")
      .append();
  parser.add_argument("files")
      .help("source files to import or open")
      .remaining();

  const auto hiddenUnlessDetailed =
      [detailed](argparse::Argument &arg) -> argparse::Argument & {
    if (!detailed) {
      arg.hidden();
    }
    return arg;
  };

  if (detailed) {
    parser.add_group("Networking and Swarm options");
  }

  hiddenUnlessDetailed(parser.add_argument("--torrent"))
      .help("a .torrent file, a magnet link naming one already given, or a "
            "name (magnet:?xs=urn:btpk:KEY); repeatable. Content referenced "
            "by the document is resolved from these")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--torrent-data"))
      .help("directory where files described by --torrent are; empty means "
            "beside each .torrent file")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--swarm"))
      .help("fetch quoted content from the BitTorrent network rather than from "
            "a disk here")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--private-dht"))
      .help("allow more than one DHT node on the same /8 network. Used for "
            "automated tests where several nodes run on 127.0.0.1")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--peer"))
      .help("introduce a peer as HOST:PORT; repeatable. Useful when two "
            "machines are testing together and have not found each other "
            "through the DHT")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--dht-node"))
      .help("bootstrap the DHT from a known node as HOST:PORT; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--collab-room"))
      .help("collaborative room name for real-time swarm editing")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--collab-host"))
      .help("collaborative room host fingerprint")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--collab-name"))
      .help("local author display name for collaborative carets")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--auto-unlock"))
      .help("automatically unlock transcopyright spans on launch")
      .default_value(false)
      .implicit_value(true);

  if (detailed) {
    parser.add_group("Batch import and orchestration options");
  }

  hiddenUnlessDetailed(parser.add_argument("--author-name"))
      .help("name to record on publications made from this machine")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--author-email"))
      .help("email to record alongside the author name")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--gpg-key"))
      .help("fingerprint of the secret key to sign authorship records with")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--author-here"))
      .help("keep the --author-* settings in this store (author.yaml) rather "
            "than in the per-user configuration (~/.config/xudu/config.yaml)")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--show-config"))
      .help("print the current configuration and quit")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--check-authorship"))
      .help("verify the signature on this publication, report who signed it, "
            "and quit. Answers 'who is this from?' without importing it into "
            "a store -- the two are different questions")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--read"))
      .help("open a published document from a manifest file; repeatable. It is "
            "taken into this store, so it can then be read, quoted and linked "
            "to by documents here -- including ones that have never been "
            "published themselves. Refused if the manifest is not signed by "
            "whoever it claims to be from")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--publish"))
      .help("publish the opening document under this name, writing a signed "
            "manifest and the torrent carrying what was typed here into "
            "<store>/published. The name is a salt under this machine's key: "
            "publishing again under the same name is a further state of the "
            "same document. Ctrl-shift-s does the same while running")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--quote"))
      .help("quote a byte range of a torrent-backed file, as "
            "FILE_INDEX,OFFSET,LENGTH; repeatable. Appended to the store's "
            "latest state")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--import"))
      .help(
          "read files into stores as initial operations; repeatable. The "
          "first file goes into the primary store (if empty), while additional "
          "files receive independent temporary stores")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--import-branch"))
      .help("import a file as a new root microversion branch in the primary "
            "store; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--import-break"))
      .help("insert a page break and append text from file into the opening "
            "document; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--insert-text"))
      .help("insert text into document as DOC:POS:FILE_OR_TEXT; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--structure-script"))
      .help("apply a sequential combined Xanadu/Zigzag store script; each "
            "line is genesis, dimension NAME, cell NAME TEXT, cell-text NAME "
            "TEXT, text TEXT, or text-append TEXT")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--transclude"))
      .help("transclude span from one doc into another as "
            "SRCDOC:START:LEN,DESTDOC:POS; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--transclude-text"))
      .help("transclude text matching query from one doc into another as "
            "SRCDOC:QUERY,DESTDOC:POS; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--format-link"))
      .help("create formatting link as DOC:START:LEN:ATTR[:TIER[:OWNER]]; "
            "repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--dimension-link"))
      .help("create dimension link as "
            "DOC1:START:LEN,DOC2:START:LEN:DIMNAME; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--permascroll"))
      .help("directory holding the sovereign user permascroll to bind; "
            "defaults to $XDG_DATA_HOME/xudu/permascroll/default")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--dump-permascroll"))
      .help("dump sovereign user permascroll bytes to a file upon exit")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--open-store"))
      .help("open an existing xanadoc store as an auxiliary document; "
            "repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--export-osmic"))
      .help("export human-readable OSMIC text spools alongside binary stores")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--headless", "--batch"))
      .help("run batch commands non-interactively and exit without GUI")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--link"))
      .help("create a link between open document spans as "
            "DOC1:START:LEN,DOC2:START:LEN[:TYPE[:TIER[:OWNER]]]; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--system-doc"))
      .help("open a sovereign system xanadoc (keymap, settings, layout, ui, "
            "pouches); repeatable")
      .append();

  if (detailed) {
    std::cout << parser << "\n";
    return 0;
  }

  render::Backend backend = render::Backend::OpenGL;
  RendererRef renderer;
  std::unique_ptr<Session> session;
  bool quiet = false;
  MicroversionId opening;
  std::string asked;
  std::string alongside;
  std::string publishAs;
  std::vector<MicroversionId> read;
  std::vector<MicroversionId> background;
  std::vector<std::pair<MicroversionId, std::size_t>> extraImports;

  try {
    parser.parse_args(argc, argv);
    if (parser["--show-config"] == true) {
      std::cout << "# " << xudu::configPath() << "\n"
                << xudu::loadConfig().toYaml();
      return 0;
    }
    if (const auto where = parser.get<std::string>("--check-authorship");
        !where.empty()) {
      return checkAuthorship(where);
    }
    const bool headless =
        parser["--headless"] == true || parser["--batch"] == true;
    quiet = parser["--print-asset-dir"] == true || headless;

    // A document holds operations naming addresses in the author's
    // permascroll and no primedia of its own, so a permascroll that does not
    // outlive the process is a document that reopens empty. It is bound before
    // the session because every store the session opens shares this one --
    // that is what makes a passage quoted from one document into another the
    // same bytes at the same address rather than a copy.
    std::shared_ptr<xudu::UserPermascroll> userPermascroll;
    if (const auto permaPath = parser.get<std::string>("--permascroll");
        !permaPath.empty()) {
      xudu::UserPermascroll::Config config;
      config.storageDir = permaPath;
      userPermascroll =
          std::make_shared<xudu::UserPermascroll>(std::move(config));
    } else {
      userPermascroll = xudu::PermascrollRegistry::instance().defaultUser();
    }

    session = std::make_unique<Session>(parser.get<std::string>("store"),
                                        userPermascroll);
    state->onDecoratedInsert = [&session](Doc &doc, const std::uint32_t at,
                                          const std::uint32_t length,
                                          const gleditor::DecorationMask mask) {
      session->markDecorated(doc, at, length, mask);
    };

    const auto batchRes =
        xudu::BatchOrchestrator::execute(*session, parser, quiet);
    if (batchRes.shouldExit) {
      return batchRes.exitCode;
    }
    opening      = batchRes.opening;
    extraImports = std::move(batchRes.extraImports);

    backend = gleditor::applyCommonArguments(parser, state, argc, argv);
    if (parser["--headless"] == true && !state->script.empty()) {
      state->profiling = true;
    }
    renderer = Renderer::create(state, backend);

    const auto collabRoom = parser.get<std::string>("--collab-room");
    if (parser["--swarm"] == true || !collabRoom.empty()) {
      if (!session->swarmEnabled()) {
        session->useSwarm(parser["--private-dht"] == true);
        quiet || std::cout << "xudu: swarm listening on port "
                           << session->swarmPort() << "\n";
      }
    }

    if (!collabRoom.empty()) {
      const auto collabHost = parser.get<std::string>("--collab-host");
      auto collabName       = parser.get<std::string>("--collab-name");
      if (collabName.empty()) {
        collabName =
            session->author().name.empty() ? "Author" : session->author().name;
      }
      const auto roomTarget =
          xudu::SwarmContentSource::collabRoomTarget(collabHost, collabRoom);
      session->setCollabRoom(roomTarget);

      const std::string myFp = session->identity().publicKey.hex();
      const std::string myScrollKey =
          "btpk:" + session->identity().publicKey.hex() + ":main";
      session->setLocalCollaboratorInfo(collabName, myFp, myScrollKey);
      quiet || std::cout << "xudu: joined collaborative room '" << collabRoom
                         << "' (target: " << roomTarget.hex() << ") as "
                         << collabName << "\n";
    }

    if (parser.present<std::vector<std::string>>("--dht-node")) {
      for (const auto &spec :
           parser.get<std::vector<std::string>>("--dht-node")) {
        const auto colon = spec.rfind(':');
        if (std::string::npos == colon) {
          throw std::runtime_error("--dht-node expects HOST:PORT, got: " +
                                   spec);
        }
        session->addDhtNode(
            spec.substr(0, colon),
            static_cast<std::uint16_t>(std::stoul(spec.substr(colon + 1))));
        quiet || std::cout << "xudu: joining the DHT through " << spec << "\n";
      }
    }

    std::vector<xudu::InfoHash> available;
    if (parser.present<std::vector<std::string>>("--torrent")) {
      const auto root = parser.get<std::string>("--torrent-data");
      for (const auto &file :
           parser.get<std::vector<std::string>>("--torrent")) {
        const auto hash = xudu::MutableLink::looksLikeMutableLink(file)
                              ? session->addName(file)
                          : xudu::MagnetLink::looksLikeMagnet(file)
                              ? session->addMagnet(file)
                              : session->addTorrent(file, root);
        available.push_back(hash);
        if (const auto *const meta = session->content().metainfo(hash);
            nullptr != meta) {
          quiet || std::cout << "xudu: " << file << " is " << meta->magnet()
                             << " (" << meta->files().size() << " file(s), "
                             << meta->totalLength() << " bytes)\n";
        } else {
          quiet || std::cout << "xudu: " << file << " is " << hash.hex()
                             << " (awaiting metadata)\n";
        }
      }
    }

    if (parser.present<std::vector<std::string>>("--peer")) {
      if (available.empty()) {
        throw std::runtime_error("--peer needs a --torrent to attach it to");
      }
      for (const auto &spec : parser.get<std::vector<std::string>>("--peer")) {
        const auto colon = spec.rfind(':');
        if (std::string::npos == colon) {
          throw std::runtime_error("--peer expects HOST:PORT, got: " + spec);
        }
        session->connectPeer(
            available.back(), spec.substr(0, colon),
            static_cast<std::uint16_t>(std::stoul(spec.substr(colon + 1))));
        quiet || std::cout << "xudu: asking " << spec << " for "
                           << available.back().hex() << "\n";
      }
    }

    if (parser.present<std::vector<std::string>>("--quote")) {
      if (available.empty()) {
        throw std::runtime_error("--quote needs a --torrent to quote from");
      }
      if (!session->awaitMetadata(available.back(), std::chrono::seconds{60})) {
        throw std::runtime_error(
            "--quote: no metadata arrived for " + available.back().hex() +
            ". A magnet or a name carries only which content is meant; the "
            "piece hashes come from a peer, so one has to be reachable -- try "
            "--peer HOST:PORT.");
      }
      for (const auto &spec : parser.get<std::vector<std::string>>("--quote")) {
        const auto first  = spec.find(',');
        const auto second = spec.find(',', first + 1);
        if (std::string::npos == first || std::string::npos == second) {
          throw std::runtime_error("--quote expects FILE,OFFSET,LENGTH, got: " +
                                   spec);
        }
        const auto fileIndex =
            static_cast<std::uint32_t>(std::stoul(spec.substr(0, first)));
        const auto offset =
            std::stoull(spec.substr(first + 1, second - first - 1));
        const auto length = std::stoull(spec.substr(second + 1));

        const auto curVer = !session->views().empty()
                                ? session->views()[0].version
                                : session->store(0).latest();
        const auto at     = static_cast<std::uint32_t>(
            session->store(0).rebuild(curVer).length());
        const auto produced = session->quoteTorrent(
            curVer, at, available.back(), fileIndex, offset, length);
        if (!session->views().empty()) {
          session->views()[0].version = produced;
          session->views()[0].pieces  = session->store(0).rebuild(produced);
        } else {
          session->viewOpened(produced, 0);
        }
        opening = produced;
        quiet || std::cout << "xudu: " << produced.str() << " quotes "
                           << available.back().hex() << " file " << fileIndex
                           << " [" << offset << "," << offset + length << ")\n";
      }
      session->save(0);
    }

    if (const auto name = parser.get<std::string>("--author-name"),
        email           = parser.get<std::string>("--author-email"),
        key             = parser.get<std::string>("--gpg-key");
        !name.empty() || !email.empty() || !key.empty()) {
      xudu::Author who{name, email, key};
      if (!who.named() && parser["--author-here"] != true) {
        const auto existing = xudu::loadConfig();
        if (!Author{name.empty() ? existing.author.name : name,
                    email.empty() ? existing.author.email : email, key}
                 .named()) {
          throw std::runtime_error(
              "--author-name and --author-email go together: an authorship "
              "record with only half of a person in it names nobody");
        }
      }
      const bool here = parser["--author-here"] == true;
      auto recorded   = who;
      if (here) {
        session->setAuthor(who);
      } else {
        auto config = xudu::loadConfig();
        if (!who.name.empty()) {
          config.author.name = who.name;
        }
        if (!who.email.empty()) {
          config.author.email = who.email;
        }
        if (!who.gpgKey.empty()) {
          config.author.gpgKey = who.gpgKey;
        }
        xudu::saveConfig(config);
        recorded = config.author;
      }
      quiet || std::cout << "xudu: publishing as " << recorded.name << " <"
                         << recorded.email << ">"
                         << (recorded.gpgKey.empty()
                                 ? std::string{}
                                 : ", signed by " + recorded.gpgKey)
                         << (here ? " from " + session->path(0)
                                  : " (kept in " + xudu::configPath() + ")")
                         << "\n";
    }

    if (parser.present<std::vector<std::string>>("--read")) {
      for (const auto &file : parser.get<std::vector<std::string>>("--read")) {
        read.push_back(session->readPublication(file));
      }
      session->save(0);
    }
    if (parser.present<std::vector<std::string>>("--background")) {
      for (const auto &verStr :
           parser.get<std::vector<std::string>>("--background")) {
        background.push_back(MicroversionId::parse(verStr));
      }
    }

    asked = parser.get<std::string>("--version-id");
    if (opening.isZero()) {
      opening = asked.empty()
                    ? (read.empty() ? session->store(0).latest() : read.front())
                    : MicroversionId::parse(asked);
    }
    alongside = parser.get<std::string>("--alongside");
    publishAs = parser.get<std::string>("--publish");
    if (!publishAs.empty()) {
      const auto manifest = session->publishDocument(
          opening, Session::PublishRequest{publishAs, publishAs, {}, {}, {}},
          0);
      quiet || std::cout << "xudu: published " << opening.str() << " as "
                         << manifest << "\n";
    }

    quiet || std::cout << "xudu " << TOSTRING(GLEDITOR_VERSION) << ": "
                       << session->store(0).opCount() << " operations in "
                       << session->path(0) << ", opening " << opening.str()
                       << "\n";
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << parser;
    return 1;
  }

  try {
    HypertimeMap map("Sans 10", *session);
    map.setVisible(parser["--map"] == true);

    PouchDrawer pouchDrawer(*session, renderer, "Sans 10");
    pouchDrawer.setOpen(parser["--pouch"] == true, false);

    if (parser.present<std::vector<std::string>>("--alias")) {
      for (const auto &spec : parser.get<std::vector<std::string>>("--alias")) {
        const auto colon = spec.find(':');
        if (colon != std::string::npos) {
          const auto verStr   = spec.substr(0, colon);
          const auto aliasStr = spec.substr(colon + 1);
          session->store(0).setVersionAnnotation(MicroversionId::parse(verStr),
                                                 {.alias       = aliasStr,
                                                  .description = {},
                                                  .tag         = {},
                                                  .timestamp   = {}});
        }
      }
    }
    if (parser.present<std::vector<std::string>>("--compare")) {
      for (const auto &spec :
           parser.get<std::vector<std::string>>("--compare")) {
        std::stringstream ss(spec);
        std::string item;
        while (std::getline(ss, item, ',')) {
          if (!item.empty()) {
            map.toggleComparison(MicroversionId::parse(item));
          }
        }
      }
    }

    ImageOverlay images("Sans 11");

    auto docSwitcher = std::make_shared<gleditor::DocumentSwitcher>("Sans 10");
    gleditor::Form publishForm("Sans 11");
    Views views(*session, renderer, map, images, publishForm, state,
                docSwitcher);

    SwarmCatalog swarmCatalog;
    SwarmTelescopeOverlay swarmTelescope(swarmCatalog, renderer, "Sans 10");
    swarmTelescope.setOnSummon([&views](const PublicationEntry &entry) {
      views.summonPublication(entry);
    });
    if (parser["--telescope"] == true) {
      swarmTelescope.setVisible(true);
    }

    state->wheelHandler = [&views](float /*wx*/, float wy,
                                   std::uint16_t /*mods*/) -> bool {
      if (!views.onionSkinMode()) {
        return false;
      }
      if (wy > 0.1F) {
        views.cycleOnionSkin(-1);
        return true;
      }
      if (wy < -0.1F) {
        views.cycleOnionSkin(1);
        return true;
      }
      return false;
    };

    pouchDrawer.setSwingBackHandler(
        [&views](const PouchItem &item) { views.swingBackToSpan(item); });

    KineticTetherEngine kineticTetherEngine;
    KineticTetherOverlay kineticTetherOverlay(kineticTetherEngine, "Sans 10");
    renderer->addFrameContributor(&kineticTetherOverlay);

    PageBreakOverlay pageBreakOverlay(*session, renderer, "Sans 10");
    renderer->addFrameContributor(&pageBreakOverlay);
    renderer->addPickObserver(&pageBreakOverlay);
    pageBreakOverlay.setOnSplit(
        [&views](const std::uint32_t docIdx, const std::uint32_t charOffset) {
          views.insertPageBreak(docIdx, charOffset);
        });

    TranscopyrightOverlay transcopyrightOverlay(*session, renderer, "Sans 10");
    renderer->addFrameContributor(&transcopyrightOverlay);
    renderer->addPickObserver(&transcopyrightOverlay);
    transcopyrightOverlay.setUnlockCallback(
        [&views](const std::size_t sIdx, const PrimediaSpan &span) {
          views.unlockTranscopyright(sIdx, span);
        });

    session->setTranscopyrightUnlockedHandler(
        [&transcopyrightOverlay](const std::size_t docIdx,
                                 const PrimediaSpan &span,
                                 const std::uint64_t cost) {
          transcopyrightOverlay.notifyUnlocked(docIdx, span, cost);
        });

    WireframeHullOverlay wireframeHullOverlay(renderer, "Sans 10");
    renderer->addFrameContributor(&wireframeHullOverlay);
    views.setWireframeOverlay(&wireframeHullOverlay);

    xudu::CollaboratorCaretOverlay collaboratorOverlay(*session, renderer,
                                                       "Sans 9");
    renderer->addFrameContributor(&collaboratorOverlay);

    kineticTetherEngine.setVoidSpawnHandler(
        [&views](const TetherPayload &payload, const float sx, const float sy) {
          views.spawnTranscludedDocument(payload, sx, sy);
        });

    state->mouseDownHandler = [&kineticTetherEngine, &session, renderer,
                               state](const int mx, const int my,
                                      const std::uint8_t button) -> bool {
      if (button != 1) {
        return false;
      }
      const auto modState = SDL_GetModState();
      const bool altHeld  = (0 != (modState & SDL_KMOD_ALT));
      bool dragStarted    = false;

      renderer->runWithState([&kineticTetherEngine, &session, &dragStarted, mx,
                              my, state, altHeld](RenderState &rState) {
        auto *const caret = rState.caret;
        if (!caret || !caret->hasSelection()) {
          return;
        }
        const auto selStart = caret->selectionStart();
        const auto selEnd   = caret->selectionEnd();
        const auto docIdx   = caret->documentIndex();
        if (docIdx >= session->views().size() || selEnd <= selStart) {
          return;
        }

        const auto &openView = session->views()[docIdx];
        const auto &st       = session->store(openView.storeIndex);
        const auto ver       = st.rebuild(openView.version);
        const auto spans     = ver.spansFor(selStart, selEnd - selStart);
        if (spans.empty()) {
          return;
        }
        const auto text = st.textOf(openView.version);
        std::string preview;
        if (selStart < text.size()) {
          preview = text.substr(selStart, std::min(selEnd - selStart, 40U));
        }

        const float screenX = static_cast<float>(mx);
        const float screenY = static_cast<float>(state->view.screenHeight - my);

        TetherPayload payload{
            .span            = spans.front(),
            .previewText     = std::move(preview),
            .originVersion   = openView.version,
            .originDocIndex  = docIdx,
            .originCharStart = selStart,
            .originCharEnd   = selEnd,
            .originScreenPos = glm::vec2(screenX, screenY),
        };

        if (altHeld) {
          kineticTetherEngine.startDrag(std::move(payload), screenX, screenY);
          dragStarted = true;
        }
      });

      return dragStarted;
    };

    state->mouseMotionHandler = [&kineticTetherEngine, &session, renderer,
                                 state](const int mx, const int my,
                                        const std::uint32_t buttons) -> bool {
      const float screenX = static_cast<float>(mx);
      const float screenY = static_cast<float>(state->view.screenHeight - my);

      if (kineticTetherEngine.isDragging()) {
        kineticTetherEngine.updateDrag(screenX, screenY);
        return true;
      }

      if (0 != (buttons & SDL_BUTTON_LMASK)) {
        const auto modState = SDL_GetModState();
        if (0 != (modState & SDL_KMOD_ALT)) {
          renderer->runWithState([&kineticTetherEngine, &session, screenX,
                                  screenY](RenderState &rState) {
            auto *const caret = rState.caret;
            if (!caret || !caret->hasSelection()) {
              return;
            }
            const auto selStart = caret->selectionStart();
            const auto selEnd   = caret->selectionEnd();
            const auto docIdx   = caret->documentIndex();
            if (docIdx >= session->views().size() || selEnd <= selStart) {
              return;
            }
            const auto &openView = session->views()[docIdx];
            const auto &st       = session->store(openView.storeIndex);
            const auto ver       = st.rebuild(openView.version);
            const auto spans     = ver.spansFor(selStart, selEnd - selStart);
            if (spans.empty()) {
              return;
            }
            const auto text = st.textOf(openView.version);
            std::string preview;
            if (selStart < text.size()) {
              preview = text.substr(selStart, std::min(selEnd - selStart, 40U));
            }
            TetherPayload payload{
                .span            = spans.front(),
                .previewText     = std::move(preview),
                .originVersion   = openView.version,
                .originDocIndex  = docIdx,
                .originCharStart = selStart,
                .originCharEnd   = selEnd,
                .originScreenPos = glm::vec2(screenX, screenY),
            };
            kineticTetherEngine.startDrag(std::move(payload), screenX, screenY);
          });
          if (kineticTetherEngine.isDragging()) {
            return true;
          }
        }
      }

      return false;
    };

    state->mouseUpHandler =
        [&pouchDrawer, &kineticTetherEngine, &session, renderer,
         state](const int mx, const int my, const std::uint8_t button) -> bool {
      if (button != 1) { // 1 = SDL_BUTTON_LEFT
        return false;
      }
      const float screenX = static_cast<float>(mx);
      const float screenY = static_cast<float>(state->view.screenHeight - my);

      // 1. If kinetic tether is currently dragging:
      if (kineticTetherEngine.isDragging()) {
        if (pouchDrawer.isOpen() && pouchDrawer.currentWidth() >= 50.0F) {
          const bool hitZone =
              (pouchDrawer.zoneAt(screenX, screenY) != nullptr);
          const bool hitLeft =
              pouchDrawer.forge().containsLeft(screenX, screenY);
          const bool hitRight =
              pouchDrawer.forge().containsRight(screenX, screenY);
          if (hitZone || hitLeft || hitRight) {
            const auto &payload = kineticTetherEngine.payload();
            pouchDrawer.handleGhostDrop(
                payload.span, payload.previewText, payload.originVersion,
                screenX, screenY, payload.originDocIndex,
                payload.originCharStart, payload.originCharEnd);
            kineticTetherEngine.cancelDrag();
            return true;
          }
        }
        kineticTetherEngine.endDrag(screenX, screenY);
        return true;
      }

      // 2. Direct drop into open pouch drawer from selection:
      if (!pouchDrawer.isOpen() || pouchDrawer.currentWidth() < 50.0F) {
        return false;
      }
      const bool hitZone  = (pouchDrawer.zoneAt(screenX, screenY) != nullptr);
      const bool hitLeft  = pouchDrawer.forge().containsLeft(screenX, screenY);
      const bool hitRight = pouchDrawer.forge().containsRight(screenX, screenY);

      if (!hitZone && !hitLeft && !hitRight) {
        return false;
      }

      renderer->runWithState([&pouchDrawer, &session, screenX,
                              screenY](RenderState &rState) {
        auto *const caret = rState.caret;
        if (!caret || !caret->hasSelection()) {
          return;
        }
        const auto selStart = caret->selectionStart();
        const auto selEnd   = caret->selectionEnd();
        const auto docIdx   = caret->documentIndex();
        if (docIdx >= session->views().size()) {
          return;
        }
        const auto &openView = session->views()[docIdx];
        const auto &st       = session->store(openView.storeIndex);
        const auto ver       = st.rebuild(openView.version);
        if (selEnd <= selStart) {
          return;
        }
        const auto spans = ver.spansFor(selStart, selEnd - selStart);
        if (spans.empty()) {
          return;
        }
        const auto text = st.textOf(openView.version);
        std::string preview;
        if (selStart < text.size()) {
          preview = text.substr(selStart, std::min(selEnd - selStart, 40U));
        }
        pouchDrawer.handleGhostDrop(spans.front(), preview, openView.version,
                                    screenX, screenY, docIdx, selStart, selEnd);
      });
      return true;
    };

    docSwitcher->setCloseHandler([&views](const std::uint32_t docIndex) {
      views.closeDocument(docIndex);
    });
    docSwitcher->setNewDocHandler([&views]() { views.newDocument(); });
    docSwitcher->setSelectHandler([&renderer](const std::uint32_t docIndex) {
      renderer->runWithState([&renderer, docIndex](RenderState &rState) {
        if (docIndex < rState.docs.size() && rState.docs[docIndex]) {
          auto *const caret = renderer->editCaret();
          if (caret) {
            caret->placeAt(docIndex, 0);
          }
        }
      });
    });

    auto radialMenu     = std::make_shared<gleditor::RadialMenu>("Sans 10");
    const auto &uiStore = session->systemStore(xudu::SystemDocKind::UI);
    if (uiStore.opCount() > 0) {
      radialMenu->setConfig(xudu::UIConfig::fromStore(uiStore).radialMenu);
    }

    radialMenu->setActionHandler(
        [&session, &views](const std::string &id, const std::string &action,
                           const std::uint32_t docIndex,
                           const std::uint32_t charOffset,
                           const std::uint32_t charLength) {
          std::cout << "xudu: radial action: id=" << id << " action=" << action
                    << " doc=" << docIndex << " offset=" << charOffset
                    << " len=" << charLength << "\n";
          if (id == "format:bold") {
            session->markDecorated(
                docIndex, charOffset, charLength,
                gleditor::decorationBit(gleditor::Decoration::Bold));
          } else if (id == "format:italic") {
            session->markDecorated(
                docIndex, charOffset, charLength,
                gleditor::decorationBit(gleditor::Decoration::Italic));
          } else if (id == "format:underline") {
            session->markDecorated(
                docIndex, charOffset, charLength,
                gleditor::decorationBit(gleditor::Decoration::Underline));
          } else if (id == "format:superscript") {
            session->markDecorated(
                docIndex, charOffset, charLength,
                gleditor::decorationBit(gleditor::Decoration::Superscript));
          } else if (id == "format:subscript") {
            session->markDecorated(
                docIndex, charOffset, charLength,
                gleditor::decorationBit(gleditor::Decoration::Subscript));
          } else if (id == "align:left") {
            session->setAlignment(docIndex, charOffset, charLength,
                                  gleditor::TextAlign::Left);
          } else if (id == "align:centre" || id == "align:center") {
            session->setAlignment(docIndex, charOffset, charLength,
                                  gleditor::TextAlign::Centre);
          } else if (id == "align:right") {
            session->setAlignment(docIndex, charOffset, charLength,
                                  gleditor::TextAlign::Right);
          } else if (id == "align:justify") {
            session->setAlignment(docIndex, charOffset, charLength,
                                  gleditor::TextAlign::Justify);
          } else if (id == "op:pagebreak") {
            session->insertBreak(docIndex, charOffset);
          } else if (id == "op:transclude") {
            views.transcludeSelection();
          } else if (id == "info:author") {
            std::string authorStr = "Local Sovereign Author";
            if (const auto *ps = session->userPermascroll()) {
              authorStr = std::format("Author OpenPGP: {}",
                                      ps->config().masterIdentity.view());
            }
            std::cout << "xudu: " << authorStr << "\n";
          }
        });

    LinkBeams links(*session, renderer);
    links.setVisible(parser["--no-beams"] != true);
    links.setSworph(parser["--no-sworph"] != true);
    if (parser["--physics"] == true || parser["--tension-layout"] == true) {
      links.setPhysicsEnabled(true);
    }
    TenuousTetherOverlay tenuousTetherOverlay(renderer, nullptr);
    links.setTetherOverlay(&tenuousTetherOverlay);
    SatelloidOverlay satelloidOverlay(renderer);
    links.setSatelloidOverlay(&satelloidOverlay);
#ifdef XUZZ_BUILD
    auto zigzagPresentation =
        std::make_shared<zigzag::ZigzagVisualizer>(state->defaultFontName);
    auto &bridgeStore = session->store(0);
    zigzagPresentation->bindXuduStore(bridgeStore,
                                      bridgeStore.primaryCurrentVersion());
    zigzagPresentation->setPresentationConfig(
        xudu::LayoutConfig::fromStore(
            session->systemStore(xudu::SystemDocKind::Layout))
            .zigzag);
    // Derive the structural presentation's placement from the live Xanadoc
    // page, so edits, reflow, and document motion keep the two together.
    zigzagPresentation->setPresentationTransformResolver(
        [&views] { return views.presentationTransform(); });
    xudu::BridgeCoordinator bridgeCoordinator(links, renderer,
                                              *state->accessibility);
    bridgeCoordinator.connectSatelloidNavigation(satelloidOverlay);
    bridgeCoordinator.attach(*zigzagPresentation);
#endif
    links.setOpener([&views](const MicroversionId &version) {
      views.showAlongside(version);
    });
    links.setMediaRectResolver(
        [&images, &views, &session](
            const Doc &doc, const std::size_t storeIndex,
            const MicroversionId &version,
            const std::uint32_t docOffset) -> std::optional<Doc::Anchor> {
          const auto spans = session->mediaSpansFor(version, storeIndex);
          for (const auto &mSpan : spans) {
            // Every span's anchor is the same fixed-width U+FFFC run now
            // (see Session::sourceFor()'s own comment), so the span this
            // offset falls in is a span whose anchor starts at or before it
            // and ends within 3 bytes -- no per-span reserved length to look
            // up anymore.
            if (docOffset < mSpan.docOffset ||
                docOffset >= mSpan.docOffset + 3) {
              continue;
            }
            if (mSpan.isImage) {
              return images.rectFor(doc, mSpan.docOffset);
            }
            return views.widgetRectFor(doc, mSpan.docOffset);
          }
          return std::nullopt;
        });

    renderer->addSpanDecorator(session.get());
    renderer->addFrameContributor(docSwitcher.get());
    renderer->addFrameContributor(&map);
    renderer->addFrameContributor(&links);
    renderer->addFrameContributor(&tenuousTetherOverlay);
    renderer->addFrameContributor(&satelloidOverlay);
    renderer->addPickObserver(&satelloidOverlay);
    renderer->addFrameContributor(&images);
    renderer->addFrameContributor(&views);
    renderer->addFrameContributor(radialMenu.get());

    state->accessibility->addSource(docSwitcher.get());
    state->accessibility->addSource(&links);
    state->accessibility->addSource(&map);
    state->accessibility->addSource(&publishForm);
    state->accessibility->addSource(radialMenu.get());
    state->accessibility->addSource(&pouchDrawer);
    state->accessibility->setToolkit("gleditor", TOSTRING(GLEDITOR_VERSION));

    map.setGoer([&views](const MicroversionId &id) { views.showOnly(id); });
    map.setScrubHandler(
        [&views](const MicroversionId &id) { views.showOnly(id); });
    map.setCompareHandler(
        [&views, &session, &renderer](const std::vector<MicroversionId> &vers) {
          const auto count = session->views().size();
          for (std::size_t i = 0; i < count; i++) {
            renderer->push(RenderItemCloseDoc());
          }
          renderer->runWithState(
              [&session](RenderState &) { session->clearViews(); });
          for (const auto &v : vers) {
            views.showAlongside(v, 0.0F, 0);
          }
        });
    map.setOnionSkinHandler(
        [&views, &session, &renderer](const std::vector<MicroversionId> &vers) {
          const auto count = session->views().size();
          for (std::size_t i = 0; i < count; i++) {
            renderer->push(RenderItemCloseDoc());
          }
          renderer->runWithState(
              [&session](RenderState &) { session->clearViews(); });
          for (const auto &v : vers) {
            views.showAlongside(v, 0.0F, 0);
          }
          views.setOnionSkin(true);
        });
    map.setQuoteHandler([&session, &views](const MicroversionId &srcVer,
                                           const std::uint32_t srcAt,
                                           const std::uint32_t srcLen) {
      if (session->views().empty()) {
        return;
      }
      auto &st           = session->store(0);
      const auto headVer = session->views().front().version;
      const auto headLen =
          static_cast<std::uint32_t>(st.textOf(headVer).size());
      st.transclude(headVer, headLen, srcVer, srcAt, srcLen);
      views.showOnly(headVer);
    });
    renderer->addFrameContributor(&publishForm);
    renderer->addFrameContributor(&pouchDrawer);
    renderer->addFrameContributor(&swarmTelescope);
    state->modal = &publishForm;
    renderer->addPickObserver(docSwitcher.get());
    renderer->addPickObserver(&links);
    renderer->addPickObserver(radialMenu.get());
    renderer->addPickObserver(&map);
    renderer->addPickObserver(&pouchDrawer);
    renderer->addPickObserver(&swarmTelescope);
    if (asked.empty() && read.empty() && alongside.empty() &&
        extraImports.empty()) {
      const auto &primaryStore = session->store(0);
#ifdef XUZZ_BUILD
      // Xuzz composes the current Xanadoc beside the current manifold. Its
      // historical operations are navigation material, not nine overlapping
      // document planes at startup.
      views.showAlongside(primaryStore.primaryCurrentVersion(), 0.0F, 0);
#else
      const auto allVers = primaryStore.allVersions();
      if (allVers.size() > 1) {
        for (std::size_t vIdx = 0; vIdx < allVers.size(); ++vIdx) {
          views.showAlongside(allVers[vIdx], 0.0F, 0);
        }
      } else {
        views.showAlongside(opening, 0.0F, 0);
      }
#endif
    } else {
      views.showAlongside(opening, 0.0F, 0);
    }
    for (const auto &[extraVer, sIdx] : extraImports) {
      views.showAlongside(extraVer, 0.0F, sIdx);
    }
    if (!alongside.empty()) {
      views.showAlongside(MicroversionId::parse(alongside), 0.0F, 0);
    }
    for (const auto &also : read) {
      if (also != opening) {
        views.showAlongside(also, 0.0F, 0);
      }
    }
    for (const auto &behind : background) {
      views.showAlongside(behind, backgroundDepthZ, 0);
    }
    if (parser["--onion-skin"] == true) {
      views.setOnionSkin(true);
    }

    std::vector<std::shared_ptr<gleditor::AudioWidget>> audioWidgets;
    if (parser.present<std::vector<std::string>>("--audio")) {
      for (const auto &mrl : parser.get<std::vector<std::string>>("--audio")) {
        auto w = std::make_shared<gleditor::AudioWidget>("Sans 11");
        if (mrl == "white-noise" || mrl == "test") {
          std::vector<std::byte> dummy(1024, std::byte{0x55});
          auto stream =
              std::make_shared<gleditor::MemoryMediaStream>(std::move(dummy));
          w->load(gleditor::MediaResource::fromStream(stream,
                                                      "White Noise (48 kHz)"));
        } else {
          w->load(gleditor::MediaResource::fromFile(mrl));
        }
        w->setTitle(std::filesystem::path(mrl).filename().string());
        w->setVisible(true);
        renderer->addFrameContributor(w.get());
        renderer->addPickObserver(w.get());
        state->accessibility->addSource(w.get());
        audioWidgets.push_back(w);
      }
    }

    std::vector<std::shared_ptr<gleditor::MediaWidget>> videoWidgets;
    if (parser.present<std::vector<std::string>>("--video")) {
      for (const auto &mrl : parser.get<std::vector<std::string>>("--video")) {
        auto w = std::make_shared<gleditor::MediaWidget>("Sans 11");
        if (mrl == "test" || mrl == "pattern") {
          std::vector<std::byte> dummy(2048, std::byte{0xAA});
          auto stream =
              std::make_shared<gleditor::MemoryMediaStream>(std::move(dummy));
          w->load(gleditor::MediaResource::fromStream(stream,
                                                      "Sample Video (1080p)"));
        } else {
          w->load(gleditor::MediaResource::fromFile(mrl));
        }
        w->setTitle(std::filesystem::path(mrl).filename().string());
        w->setVisible(true);
        renderer->addFrameContributor(w.get());
        renderer->addPickObserver(w.get());
        state->accessibility->addSource(w.get());
        videoWidgets.push_back(w);
      }
    }

    renderer->runWithState([&audioWidgets, &videoWidgets](RenderState &rState) {
      if (!rState.docs.empty()) {
        for (std::size_t i = 0; i < audioWidgets.size(); ++i) {
          const auto dIdx = std::min(i, rState.docs.size() - 1);
          if (rState.docs[dIdx]) {
            audioWidgets[i]->attachToPage(rState.docs[dIdx], 0, 30.0F,
                                          110.0F +
                                              static_cast<float>(i) * 140.0F);
            audioWidgets[i]->setSize(340.0F, 120.0F);
          }
        }
        for (std::size_t i = 0; i < videoWidgets.size(); ++i) {
          // Command-line video is an operational card, not a document span:
          // keep it in screen space so it remains reachable while the first
          // document is loading and automation has stable click coordinates.
          videoWidgets[i]->setScreenPosition(
              30.0F, 80.0F + static_cast<float>(i) * 200.0F);
          videoWidgets[i]->setSize(340.0F, 180.0F);
        }
      }
    });

    gleditor::Application app(state, renderer, backend, "Xudu");
    bindCommands(app, state, views, map, links, *session, radialMenu, renderer,
                 pouchDrawer, swarmTelescope,
                 publishAs.empty() ? std::string{"document"} : publishAs);
    quiet || std::cout << "commands:\n" << app.commands().helpText();

    session->setSystemDocChangedCallback([&app, radialMenu, docSwitcher,
                                          &pouchDrawer, &links, &map
#ifdef XUZZ_BUILD
                                          ,
                                          &zigzagPresentation
#endif
    ](const xudu::SystemDocKind kind, const xudu::Store &store) {
      std::cout << "xudu: system doc updated (" << xudu::systemDocUri(kind)
                << ")\n";
      const auto model = xudu::SystemStoreModel::fromStore(store);
      if (!model.isValid()) {
        std::cerr << "xudu: rejecting invalid system store "
                  << xudu::systemDocUri(kind) << ": " << model.validationError()
                  << "\n";
        return;
      }
      switch (kind) {
      case xudu::SystemDocKind::Keymap: {
        const auto kmCfg = xudu::KeymapConfig::fromStore(store);
        for (const auto &[act, comboStr] : kmCfg.bindings) {
          if (const auto combo = gleditor::parseKeyCombo(comboStr)) {
            app.commands().rebind(act, combo->first, combo->second);
          }
        }
        break;
      }
      case xudu::SystemDocKind::Settings: {
        break;
      }
      case xudu::SystemDocKind::Layout: {
        const auto layout = xudu::LayoutConfig::fromStore(store);
        links.setVisible(layout.xanalinkRibbons);
        links.setBeamConfig(layout.beams);
        links.tensionEngine().setParams(layout.physics.toTensionParams());
        pouchDrawer.setDockSide(layout.pouchDock == xudu::PouchDock::Left
                                    ? xudu::PouchDrawer::DockSide::Left
                                    : xudu::PouchDrawer::DockSide::Right);
#ifdef XUZZ_BUILD
        zigzagPresentation->setPresentationConfig(layout.zigzag);
#endif
        break;
      }
      case xudu::SystemDocKind::UI: {
        const auto uiCfg = xudu::UIConfig::fromStore(store);
        radialMenu->setConfig(uiCfg.radialMenu);
        docSwitcher->setVisible(uiCfg.tabBarVisible);
        map.setVisible(uiCfg.hypertimeMapVisible);
        break;
      }
      case xudu::SystemDocKind::Pouches:
      case xudu::SystemDocKind::Count:
        break;
      }
    });

    // Apply active system doc configurations at launch
    {
      const auto kmIdx = session->systemStoreIndex(xudu::SystemDocKind::Keymap);
      const auto &kmStore = session->store(kmIdx);
      if (kmStore.opCount() > 0) {
        const auto kmCfg = xudu::KeymapConfig::fromStore(kmStore);
        for (const auto &[act, comboStr] : kmCfg.bindings) {
          if (const auto combo = gleditor::parseKeyCombo(comboStr)) {
            app.commands().rebind(act, combo->first, combo->second);
          }
        }
      }
      const auto uiIdx    = session->systemStoreIndex(xudu::SystemDocKind::UI);
      const auto &uiStore = session->store(uiIdx);
      if (uiStore.opCount() > 0) {
        const auto uiCfg = xudu::UIConfig::fromStore(uiStore);
        radialMenu->setConfig(uiCfg.radialMenu);
        docSwitcher->setVisible(uiCfg.tabBarVisible);
        map.setVisible(uiCfg.hypertimeMapVisible);
      }
      const auto loIdx = session->systemStoreIndex(xudu::SystemDocKind::Layout);
      const auto &loStore = session->store(loIdx);
      if (loStore.opCount() > 0) {
        const auto loCfg = xudu::LayoutConfig::fromStore(loStore);
        links.setVisible(loCfg.xanalinkRibbons);
        links.setBeamConfig(loCfg.beams);
        links.tensionEngine().setParams(loCfg.physics.toTensionParams());
        pouchDrawer.setDockSide(loCfg.pouchDock == xudu::PouchDock::Left
                                    ? xudu::PouchDrawer::DockSide::Left
                                    : xudu::PouchDrawer::DockSide::Right);
#ifdef XUZZ_BUILD
        zigzagPresentation->setPresentationConfig(loCfg.zigzag);
#endif
      }
    }

    const auto status = app.run();
    session->saveAll();
    if (parser["--export-osmic"] == true) {
      session->saveOsmicTextAll();
    }
    if (const auto outPerma = parser.get<std::string>("--dump-permascroll");
        !outPerma.empty()) {
      session->dumpPermascroll(outPerma);
    }
    return status;
  } catch (const std::exception &err) {
    std::cerr << "Error: " << err.what() << "\n";
    return 1;
  }
}

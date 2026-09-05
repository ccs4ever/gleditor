# Xudu Pouch Drawer, Partitioned Drop Zones & Clasp Assembly Bench

## 1. Executive Summary & Nelsonian Conceptual Lineage

This specification establishes the architectural design, interaction model, and
C++23 implementation for the **Pouch Drawer**, **Partitioned Drop Zones**, and
the **Clasp Assembly Bench** in `apps/xudu`.

Traditional desktop text editors rely on the Xerox PARC "clipboard" — an
amnesiac, destructive bit-bucket that severs provenance, destroys context, and
forces raw byte duplication. In Project Xanadu, Theodor Holm Nelson rejected the
clipboard in favor of persistent, addressable collections of virtual spans
(`vspans`) that preserve immutable primedia provenance.

```
+------------------------------------------------------------------------------------+
| Active Xanadoc (Z = 0)                   | POUCH DRAWER (Screen Bezel Dock)        |
+------------------------------------------+-----------------------------------------+
|                                          | [✕] POUCH DOCK        [ ⇕ Pin / Float ] |
|  Paragraph 1 with premise...             +-----------------------------------------+
|                                          | CLASP ASSEMBLY BENCH                    |
|  ⟦ Span A1: "Premise 1" ⟧                | [ Left: 2 Spans ] ──> [ 💬 Comment ▼ ]  |
|         \                                |                      └──> [ Right: 1 ]  |
|          \  (Elastic Tether Arc)         | [ FORGE CLASP ⚡ ]                      |
|           \                              +-----------------------------------------+
|            v                             | DROP ZONE: "To Link (Left)" [Cyan Aura] |
|                                          | ⟦ A1 ⟧ "Premise 1..."              [✕]  |
|                                          | ⟦ A2 ⟧ "Premise 2..."              [✕]  |
|                                          +-----------------------------------------+
|                                          | DROP ZONE: "To Link (Right)" [Magenta]  |
|                                          | ⟦ B1 ⟧ "Counterargument..."        [✕]  |
|                                          +-----------------------------------------+
|                                          | DROP ZONE: "Notes for Later" [Gold]     |
|                                          | ⟦ N1 ⟧ "Interesting quote..."      [✕]  |
|                                          | [+ New Drop Zone Partition]             |
+------------------------------------------------------------------------------------+
```

### 1.1 Nelsonian Terminology Reference

The design directly grounds modern spatial GUI interactions in authentic
Nelsonian hypertext concepts (*Literary Machines*, *Possiplex*, *CosmicBook*):

| Feature Concept | Ted Nelson's Term | Theoretical Definition |
| :--- | :--- | :--- |
| **Ghost Spanables** | **Ghostings / Ghost Spans** | Visible representations of invariant primedia characters appearing in multiple views without byte duplication (*Literary Machines*). |
| **Drop Zone Drawer** | **Pouches** | Open-ended personal collections of addresses, spans, and bookmarks carried across the docuverse (*Literary Machines* Ch. 2, FEBE Protocol). |
| **Multi-Span Bundles** | **Clutches** | A grasped bundle of pointers or spans held together for a compound operation (*Literary Machines* Glossary). |
| **3-Part Link Control** | **Clasp Assembly Bench** | A *clasp* binds $N$ homeward spans to $M$ toward spans with explicit typing (*Literary Machines* 87.1). |
| **Context Recovery** | **Sworph / Collateral Alignment** | Smoothly swinging and morphing documents side by side so related spans align at identical elevations (*Possiplex*). |

______________________________________________________________________

## 2. The Tripartite Dialectic

```
       +-------------------------------------------------------------+
       |                  Xanadulogical Purist                       |
       |  - Pouch Document: First-class floating xanadoc (EDL).      |
       |  - Clasp Assembly: N homeward spans to M toward spans.      |
       |  - Aperture Expansion: Scrub outward to reveal context.     |
       |  - Ambient Optical Filaments: Living web of connections.    |
       |  - Non-Destructive Limbo: Cleared spans remain in history.  |
       +-------------------------------------------------------------+
                                   ▲   │
                     Nelsonian     │   │ Real-World
                     Ideals        │   │ Reality Check
                                   │   ▼
       +-------------------------------------------------------------+
       |                    Systems Realist                          |
       |  - 120 FPS / 8.33ms Budget: Retained Sliced Canvas.         |
       |  - 1 GPU Draw Call: Quads and glyphs pack into Doc::VBORow. |
       |  - Fitts's Law Target: Screen bezel infinite depth (Wx->inf)|
       |  - Ultra-Wide Display: Proximity docking & radial shortcut. |
       |  - Sworph over Camera Flight: Bring document to user (0.62s)|
       +-------------------------------------------------------------+
                                   ▲   │
                     Hardware      │   │ Architectural
                     Feasibility   │   │ Integration
                                   │   ▼
       +-------------------------------------------------------------+
       |                   Codebase Expert                           |
       |  - xudu::Store: Reuses author's shared UserPermascroll.     |
       |  - ClickableRegistry: Dedicated overlay tag base 0x70000000 |
       |  - LinkBeams: Session::addLink() triggers 3D ribbons.       |
       |  - Swing-Back: Version::occurrencesOf + centroidAlignment.  |
       |  - Concrete C++23: PouchDrawer, DropZone, LinkForgeWidget.  |
       +-------------------------------------------------------------+
```

______________________________________________________________________

## 3. The Pouch Drawer Architecture & Screen Bezel Ergonomics

### 3.1 Screen Bezel Infinite Depth & Fitts's Law

- **Horizontal Targeting ($W_x \\to \\infty$)**: The physical monitor edge stops
  the cursor. Users can flick ballistic mouse movements toward the screen
  boundary with zero deceleration penalty ($MT = a + b \\log_2(2D / W)$).
- **Vertical Partitioning ($W_y < \\infty$)**: Drop zones partition the vertical
  edge into functional bands ($H \\approx 90 - 140,\\text{px}$). Corner zones
  (top and bottom) benefit from 2D infinite boundary stops.
- **Docking Flexibility**: The drawer can be docked to the Left bezel, Right
  bezel, or detached into a floating 3D panel. On ultra-wide monitors
  ($> 2560,\\text{px}$ width), the drawer automatically docks to the bezel
  closest to the active editing viewport.

### 3.2 Partitioned Drop Zones

Users can instantiate and name custom drop partitions:

1. `ToLinkLeft` (Cyan accent aura `#06B6D4`): Staging source spans for link creation.
1. `ToLinkRight` (Magenta accent aura `#EC4899`): Staging target spans for link creation.
1. `NotesForLater` (Identity Gold aura `#EAB308`): Temporary research snippets.
1. `DraftScraps` (Emerald aura `#10B981`): Uncommitted candidate phrasings.
1. `Custom` (User-defined label and color).

### 3.3 Ghost Spanable Drag Interactions

1. **Detachment**: Holding `Alt` while dragging text or pulling the `⎘` handle
   detaches a semi-transparent ghost quad.
1. **Elastic Spring Tether**: A Hookean spring tether with damping connects the
   origin span to the cursor:
   $$\\mathbf{F}_{\\text{tether}} = -k (\\mathbf{P}_{\\text{cursor}} - \\mathbf{P}\_{\\text{src}}) - c \\mathbf{v}$$
1. **Drop Routing**:
   - Dropping on a **Drop Zone** (`tagKindOverlay`): Ingests the span into that
     zone's persistent collection; the tether snaps into the drawer card.
   - Dropping in the **Void** (`tagKindNone`): Spawns a new xanadoc page quad.
   - Dropping on a **Document Page** (`tagKindGlyph`/`tagKindPage`): Transcludes
     the span inline into the target concatext.

______________________________________________________________________

## 4. The System-Xanadoc Storage Model

### 4.1 Zero Raw Byte Duplication

The Pouch Drawer is **not** an ephemeral GUI array. It is backed by a dedicated
`xudu::Store` instance sharing the author's primary `UserPermascroll`:

```cpp
auto pouchStore = std::make_unique<xudu::Store>(session.userPermascrollPtr());
```

When a span is dropped into the drawer:

- **Zero bytes are written to primedia spools**.
- The operation appends a 64-byte `CompactOpNode` record (`OpKind::Transclude`
  or `OpKind::Insert` with pre-existing `PrimediaSpan`).
- A drawer holding 50 transcluded spans consumes barely $1.2,\\text{KB}$ of RAM.

### 4.2 Hypertime Microversion Partitioning & Limbo

Each drop zone in the drawer maps to an independent microversion branch:

```
(v0:genesis) ──┬──> (v1:to_link_left)   [Left Anchor Staging]
               ├──> (v1.1:to_link_right) [Right Anchor Staging]
               └──> (v1.2:notes_later)   [Persistent Notes]
```

- **Non-Destructive Limbo**: When the user dismisses an item (`✕`) or clears a
  zone after link forging, the span is **rearranged to OSMIC limbo**, never
  deleted. Users can scrub backward in hypertime to recover any previously
  harvested span.

______________________________________________________________________

## 5. The Clasp Assembly Bench ($N \\times M$ Linking)

The Clasp Assembly Bench sits at the top of the drawer (or can be summoned into
3D space between two documents):

```
+------------------------------------------------------------------------------------+
|                                THE CLASP BENCH                                     |
+------------------------------+--------------------+--------------------------------+
| HOMESTEAD / LEFT (Source)    | RELATION NEXUS     | TOWARD / RIGHT (Target)        |
| ⟦ Electric Cyan Brackets ⟧   | [ 💬 Comment     ▼]| ⟦ Vivid Magenta Brackets ⟧     |
| - Span A1: "Premise 1"   [✕] | Tier: [Author    ▼]| - Span B1: "Counterarg"    [✕] |
| - Span A2: "Premise 2"   [✕] |                    |                                |
| [+ Drop Spans Here]          | [ FORGE CLASP ⚡ ] | [+ Drop Spans Here]            |
+------------------------------+--------------------+--------------------------------+
```

### 5.1 Asymmetric Multi-Span Link Formation

1. The user drags one or more ghost spans into the **Homestead (Left) Drop Zone**.
1. The user navigates anywhere across the docuverse, dragging counter-spans into
   the **Toward (Right) Drop Zone**.
1. In the **Relation Nexus**, the user selects the Nelsonian link type:
   `Comment`, `Illustration`, `Disagreement`, `Authorship`, `Quotation`,
   `Dimension`, or `Format`.
1. Clicking `[ FORGE CLASP ]` commits a compound `xudu::Link` to the active
   document's store:
   ```cpp
   xudu::Link link;
   link.type = selectedType;
   link.tier = selectedTier;
   link.left = leftZone.allSpans();   // N spans
   link.right = rightZone.allSpans(); // M spans
   session.addLink(activeDocIndex, std::move(link));
   ```
1. `Session::addLink()` increments the session epoch. On the next frame,
   `LinkBeams::rebuildStrands()` generates luminous 3D ribbons fanning out
   between all left and right anchors with instance hue shifts.

______________________________________________________________________

## 6. Swing-Back Context & Collinear Sworph Navigation

When a user clicks a pouch card to inspect where a snippet came from, the editor
swings back to the original context without disorienting camera jumps.

```
[ Pouch Card Clicked ]
       │
       ▼
1. Address Resolution:
   Scan open views via `Version::occurrencesOf(item.span)`
       │
       ├───> Found in active open view `docIdx`
       │
       └───> Not open:
             Resolve via `session.versionShowing({item.span}, openVersions)`
             Summon document via background opener from Z = -40
       │
       ▼
2. Collinear Sworphing (xudu::LinkBeams / gleditor::anim):
   Source document glides forward along Z and docks beside current view
   Timing: `sworphSubject = 0.62s`
       │
       ▼
3. Visual Target Aura & Framing:
   Original span illuminates with Electric Cyan brackets `⟦...⟧` and breathing pulse
   Camera adjusts framing distance via `xudu::framingDistance` (0.70s settle)
```

### 6.1 Aperture Expansion

While viewing a pouch card or its swung-back origin, the user can scrub the
**Aperture Expansion** handle (`◀ [Aperture] ▶`) to dynamically widen the span
start and end offsets within the author's immutable primedia spool, viewing
preceding and following paragraphs without losing their staging place.

______________________________________________________________________

## 7. Systems Realist Performance Invariants (120 FPS Budget)

| Invariant | Systems Risk | Realist Implementation Architecture |
| :--- | :--- | :--- |
| **CPU Frame Budget ($< 3.5,\\text{ms}$)** | Re-shaping 50 snippets via HarfBuzz every frame takes $2.5,\\text{ms}$. | **Retained Sliced Canvas**: Drawer owns a `gleditor::Canvas` marked dirty only on item add/remove/scroll. $0.00,\\text{ms}$ CPU cost on steady frames. |
| **GPU Draw Calls** | Separate draw calls per card and text snippet thrash command buffers. | **1 Instanced Draw Call**: All background quads, aura borders, buttons, and text glyphs pack into `Doc::VBORow`. |
| **Viewport Virtualization** | Shaping 100+ off-screen snippets exhausts memory. | Only the 6–8 visible snippets in the drawer scissor rectangle are passed to `TextLayout`. |
| **Zero Disk Stalls** | Synchronous `fsync()` blocks the render thread for $5 - 80,\\text{ms}$. | Drops write to an in-memory `UncommittedOpLog`. Spools use memory-mapped `VirtualMemoryArena` flushed asynchronously. |

______________________________________________________________________

## 8. Concrete C++23 Production Class Design

Below is the complete C++23 class architecture designed for `apps/xudu/`:

### 8.1 Header: `apps/xudu/pouch_drawer.hpp`

```cpp
#ifndef XUDU_POUCH_DRAWER_HPP
#define XUDU_POUCH_DRAWER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/clickable_registry.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>

#include "xudu/core/ops.hpp"
#include "xudu/core/spool.hpp"
#include "xudu/core/store.hpp"
#include "xudu/session.hpp"

namespace xudu {

/**
 * @struct PouchItem
 * @brief An individual transcluded card stored within a drop zone.
 */
struct PouchItem {
  std::uint64_t itemId{0};
  PrimediaSpan span; // 24 bytes: {scrollId, start, length}
  std::string previewText;
  MicroversionId originVersion;
  std::uint32_t originDocIndex{0};
  std::uint32_t originCharStart{0};
  std::uint32_t originCharEnd{0};
  std::uint64_t timestampUtc{0};
};

/**
 * @enum class ZoneKind
 * @brief Semantic category of a drop partition.
 */
enum class ZoneKind : std::uint8_t {
  ToLinkLeft,
  ToLinkRight,
  NotesForLater,
  DraftScraps,
  Custom
};

/**
 * @class DropZone
 * @brief A drop partition with distinct geometry, color aura, and picking tag.
 */
class DropZone {
public:
  DropZone(std::string id, ZoneKind kind, std::string label,
           std::uint32_t auraColor);

  [[nodiscard]] const std::string &id() const noexcept { return id_; }
  [[nodiscard]] ZoneKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::string &label() const noexcept { return label_; }
  [[nodiscard]] std::uint32_t auraColor() const noexcept { return auraColor_; }

  void setRect(float x, float y, float width, float height) noexcept;
  [[nodiscard]] bool contains(float screenX, float screenY) const noexcept;

  void addItem(PouchItem item);
  bool removeItem(std::uint64_t itemId);
  void clear();

  [[nodiscard]] const std::vector<PouchItem> &items() const noexcept {
    return items_;
  }
  [[nodiscard]] std::vector<PrimediaSpan> allSpans() const;

  void setHovered(bool hovered) noexcept { isHovered_ = hovered; }
  [[nodiscard]] bool isHovered() const noexcept { return isHovered_; }

  void setTagOffset(std::uint32_t offset) noexcept { tagOffset_ = offset; }
  [[nodiscard]] std::uint32_t tagOffset() const noexcept { return tagOffset_; }

private:
  std::string id_;
  ZoneKind kind_;
  std::string label_;
  std::uint32_t auraColor_{0xFFFFFFFFU};
  float x_{0.0F};
  float y_{0.0F};
  float width_{0.0F};
  float height_{0.0F};
  bool isHovered_{false};
  std::uint32_t tagOffset_{0};
  std::vector<PouchItem> items_;
};

/**
 * @class LinkForgeWidget
 * @brief Tripartite link creation control: Left Zone, Type Selector, Right
 * Zone.
 */
class LinkForgeWidget {
public:
  explicit LinkForgeWidget(ClickableRegistry &registry, std::uint32_t tagBase);

  void setGeometry(float x, float y, float width, float height);
  void draw(gleditor::Canvas &canvas, RenderState &state);

  void dropLeft(PouchItem item);
  void dropRight(PouchItem item);
  void setLinkType(LinkType type) noexcept { selectedType_ = type; }
  void setProminenceTier(ProminenceTier tier) noexcept {
    selectedTier_ = tier;
  }

  [[nodiscard]] bool canForge() const noexcept;
  bool forge(Session &session, std::uint32_t activeDocIndex);

  DropZone &leftZone() noexcept { return leftZone_; }
  DropZone &rightZone() noexcept { return rightZone_; }

private:
  DropZone leftZone_;
  DropZone rightZone_;
  LinkType selectedType_{LinkType::Comment};
  ProminenceTier selectedTier_{ProminenceTier::Author};
  float x_{0.0F};
  float y_{0.0F};
  float width_{0.0F};
  float height_{0.0F};
  std::uint32_t tagBase_{0};
};

/**
 * @class PouchDrawer
 * @brief High-performance overlay drawer containing partitioned drop zones.
 */
class PouchDrawer : public gleditor::FrameContributor,
                    public gleditor::PickObserver,
                    public gleditor::a11y::Source {
public:
  enum class DockSide : std::uint8_t { Left, Right, Both };

  PouchDrawer(Session &session, RendererRef renderer,
              ClickableRegistry &registry, DockSide side = DockSide::Right);
  ~PouchDrawer() override;

  // FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // Accessibility
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;

  // Zone Partition Management
  DropZone &addZone(std::string id, ZoneKind kind, std::string label,
                    std::uint32_t color);
  [[nodiscard]] DropZone *zoneById(std::string_view id) noexcept;
  [[nodiscard]] DropZone *zoneAt(float screenX, float screenY) noexcept;

  // Drag Interaction Hooks
  bool handleGhostDrop(const GhostDragContext &ghost, float screenX,
                       float screenY);
  void swingBack(const PouchItem &item, RenderState &state);

  void setOpen(bool open, bool animated = true);
  [[nodiscard]] bool isOpen() const noexcept { return isOpen_; }
  void toggle() { setOpen(!isOpen_); }

  LinkForgeWidget &forge() noexcept { return *forgeWidget_; }

private:
  void initClickables();
  void layoutZones(float screenWidth, float screenHeight);

  Session &session_;
  RendererRef renderer_;
  ClickableRegistry &registry_;
  DockSide side_;
  bool isOpen_{true};
  float currentSlideWidth_{320.0F};
  float targetSlideWidth_{320.0F};

  std::unique_ptr<Store> pouchStore_;
  MicroversionId pouchVersion_;

  std::vector<std::unique_ptr<DropZone>> zones_;
  std::unique_ptr<LinkForgeWidget> forgeWidget_;
  std::unique_ptr<gleditor::Canvas> canvas_;

  std::uint32_t tagBase_{0x70000000U};
  std::uint64_t a11yRevision_{1};
  std::uint64_t nextItemId_{1};
};

} // namespace xudu

#endif // XUDU_POUCH_DRAWER_HPP
```

______________________________________________________________________

## 9. Cross-Specification References

- [`design/ui_workflow_xudu_intertwingle.md`](ui_workflow_xudu_intertwingle.md):
  The master specification for 3-way spring layout physics, flying pages, and
  general spatial UI workflows.
- [`design/3d-optical-beams-and-spatial-framing.md`](3d-optical-beams-and-spatial-framing.md):
  The rendering pipeline for volumetric transclusion quads, link ribbons, and
  `bypassRoute()` obstacle avoidance.
- [`design/btfs-and-permascrolls.md`](btfs-and-permascrolls.md):
  The underlying sovereign permascroll storage architecture, 64 KiB piece
  alignment, and BEP 46 publication model.

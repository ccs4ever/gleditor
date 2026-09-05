# Xudu Intertwingled UI Design Workflow & 3-Way Tension Specification

## 1. Executive Summary

This specification establishes the comprehensive UI/UX architecture and agentic workflow for `apps/xudu` — Ted Nelson's vision of an open xanadoc and xanalogical universe where documents, raw primedia sources, and link packages intertwingle without artificial isolation walls.

```
       [ Parent Source A ]  <-- (Tenuous tether ribbons maintain origin connection)
          \           /
           \         /  (Smooth Flying Arc)
            v       v
      +-------------------+      Active Link Beam (Luminous Core Glow)      +-------------------+
      |  Active Xanadoc   |================================================>|  Transcluded Doc  |
      |  (Collinear X/Y)  |                                                 |  (Linked Page)    |
      +-------------------+                                                 +-------------------+
```

______________________________________________________________________

## 2. Wall-less Intertwingle Concept & Ambient Provenance

### 2.1 Wall-less Shared 3D Cosmos

- Traditional editors sequester text into isolated windows and modal files. In `xudu`, all xanadocs, transcluded source passages, and link packages inhabit a unified continuous 3D coordinate space.
- Documents and sources do not need explicit "File -> Open" actions to exist; they materialize automatically as they are referenced or transcluded by open views.

### 2.2 Unobtrusive Provenance & Active Indicators

- **Minimal Indicator**: Heavy titlebars and window chrome are eliminated. The active document/source is indicated by:
  - An ambient glowing boundary aura (soft 2px accent gradient with low-frequency breathing pulse).
  - A compact top-edge provenance tag displaying the microversion and spool hash (e.g. `OSMIC@v4:7e2b`).
- **Edit Tracking**:
  - Each edit is tagged in the underlying `Store` with its microversion op sequence.
  - In the UI, newly typed text is distinguished by author tinting, which fades over time to the default text color once blessed/committed into the immutable spool.

______________________________________________________________________

## 3. The 3-Way Tension Layout Engine

Documents and sources in `xudu` are positioned by a real-time continuous physics optimization solver balancing three competing priorities:

```
                          [ 1. Text Readability ]
                                   /\
                                  /  \
                                 /    \
                                /  ⚛   \
    [ 2. Link Collinear Alignment ] ---- [ 3. Global Aesthetic Harmony ]
```

### 3.1 Mathematical Energy Formulation

The total layout energy is defined as:
$$E\_{\\text{total}} = w\_{\\text{read}} E\_{\\text{read}} + w\_{\\text{align}} E\_{\\text{align}} + w\_{\\text{aest}} E\_{\\text{aest}}$$

#### Priority 1: Text Readability ($E\_{\\text{read}}$)

- Preserves comfortable viewing angles, orthogonal orientation to the camera view vector, and prevents document overlapping:
  $$E\_{\\text{read}} = \\sum_i \\frac{1}{2} k\_{\\text{cam}} |\\mathbf{P}_i - \\mathbf{P}_{\\text{cam_opt}}|^2 + \\sum\_{i \\neq j} \\frac{q\_{\\text{repel}}}{|\\mathbf{P}\_i - \\mathbf{P}_j|^2 + \\epsilon_{\\text{read}}}$$

#### Priority 2: Link Collinear Alignment & Smooth Flying Pages ($E\_{\\text{align}}$)

- When a link is clicked, hovered, or active, the destination page smoothly flies out of its resting plane into side-by-side collinear alignment ($Y_i \\approx Y_j, X_j \\approx X_i + W_i + \\text{gap}$):
  $$E\_{\\text{align}} = \\sum\_{(i, j) \\in \\text{Links}} \\text{Prominence}(i, j) \\cdot \\left[ k_y (Y_i - Y_j)^2 + k_x (X_j - X_i - D\_{\\text{opt}})^2 + k_z (Z_i - Z_j)^2 \\right]$$
- **Tenuous Connection to Parent Background**: When a linked page flies forward to align with the active focus, it remains anchored to its parent document in the background via a faint, semi-transparent elastic tether ribbon (quadratic Bezier arc with $\\alpha \\approx 0.25$).
- **Active Link Prominence**: The active link beam is rendered with full opacity, luminous core glow, and animated directional gradient energy pulse.

#### Priority 3: Global Link & Document Aesthetics ($E\_{\\text{aest}}$)

- Minimizes ribbon crossings and maintains clear depth separation between foreground active documents ($Z = 0$) and background reference corpora ($Z = -40,\\text{units}$):
  $$E\_{\\text{aest}} = \\sum\_{\\text{edges } e_1, e_2} w\_{\\text{cross}} \\cdot \\text{CrossPenalty}(e_1, e_2) + \\sum_i \\frac{1}{2} k\_{\\text{layer}} (Z_i - Z\_{\\text{tier}(i)})^2$$

______________________________________________________________________

## 4. Visual Shaders & Animation Timing

- **Link Beams Pipeline (`gleditor::Beams`)**:
  - Vertex shader builds ribbons with variable thickness and orientation.
  - Fragment shader computes distance-to-edge glow and directional gradient fade:
    $$\\text{Glow}(u) = \\exp\\left(-\\frac{(u - 0.5)^2}{2 \\sigma^2}\\right)$$
- **Animation Timings (`gleditor::anim`)**:
  - `sworphSubject` = 0.62s (flying page leads movement).
  - `sworphRow` = 0.45s (background rows make way).
  - `backgroundOpacity` = 0.42 (ambient depth cue).

______________________________________________________________________

## 5. The Tripartite Dialectic: Nelsonian Ideals vs. Silicon Reality

The architecture and interaction design of `apps/xudu` is governed by a
tripartite dialectic balancing three essential forces:

```
       +-------------------------------------------------------------+
       |                  Xanadulogical Purist                       |
       |  - Absolute Transclusion (zero copy-paste degradation)      |
       |  - Universal Docuverse & Infinite Permascrolls              |
       |  - Asymmetric N×M Multi-Span Typed Xanalinks                |
       |  - Formatting as Content-Addressed Link Attributes          |
       |  - Deep Intrinsic Bidirectionality & Zero Window Silos      |
       +-------------------------------------------------------------+
                                   ▲   │
                     Nelsonian     │   │ Real-World
                     Ideals        │   │ Reality Check
                                   │   ▼
       +-------------------------------------------------------------+
       |                    Systems Realist                          |
       |  - 120 FPS / 8.33ms GPU Frame Budget                        |
       |  - Zero Render-Thread Blocking (FUSE/DHT asynchronous)      |
       |  - Cache Lines, Memory Bandwidth & Zero-Copy mmap           |
       |  - BitTorrent v2 BEP 52 Merkle Trees & BEP 46 Mutable DHT   |
       |  - Human Cognitive Ergonomics (Fitts's Law, Visual Clutter) |
       +-------------------------------------------------------------+
                                   ▲   │
                     Hardware      │   │ Architectural
                     Feasibility   │   │ Integration
                                   │   ▼
       +-------------------------------------------------------------+
       |                   Codebase Expert                           |
       |  - `xudu::Store` (Primedia Spool + 64B CompactOpNode DAG)   |
       |  - `LinkBeams` & `Framing` (3D Beams & Volumetric Prisms)   |
       |  - `ClickableRegistry` (Multi-Kind Picking: Glyphs/Pages)   |
       |  - `ManagedTorrent`, `UserPermascroll` & `MerkleLedger`     |
       |  - Zero-Cairo/Zero-Pango FreeType 2 & HarfBuzz Text Layout  |
       |  - 3-Way Spring Physics Simulation (F_read, F_align, F_aest)|
       +-------------------------------------------------------------+
```

### 5.1 Dialectical Resolutions

#### 1. Sovereign Genesis vs. Traditional File Modals

- **Purist Position**: A document is not a named file on a disk folder.
  It is an Edit Decision List (EDL) referencing primedia in an author's
  permanent, append-only permascroll. Creating a document should instantiate
  a fresh microversion node in the universal docuverse.
- **Realist Constraint**: Modern users reject high cognitive setup friction.
  If initiating a blank page requires entering cryptographic identities,
  selecting torrent swarms, or navigating multi-step wizard modals, adoption
  fails immediately.
- **Codebase Synthesis**: `xudu::Store` automatically anchors slot 0 to the
  local author's `UserPermascroll`. Pressing `Ctrl+N` or clicking the ambient
  `+ New Xanadoc` genesis chip instantly summons a floating page quad at
  $Z = 0$ in under 16ms without disk dialogs. Typing immediately streams
  64-byte `CompactOpNode` records into the author's local 64 KiB
  page-aligned segment.

#### 2. Swarm Discovery vs. Interactive Frame Rates

- **Purist Position**: Any document must be able to link to or quote any
  text in the world docuverse without centralized web servers or siloed
  walled gardens.
- **Realist Constraint**: Decentralized BitTorrent DHT lookups and BEP 46
  key resolutions take hundreds of milliseconds to several seconds.
  Synchronous network lookups on the rendering thread immediately destroy
  the 120 FPS frame budget ($8.33,\\text{ms}$).
- **Codebase Synthesis**: `ManagedTorrent` and swarm coordinators operate on
  background worker threads via non-blocking channels. When an external
  xanadoc is summoned via the Docuverse Telescope (`Ctrl+O`), a placeholder
  blueprint quad materializes in 3D space with an ambient shimmer. Primedia
  pieces stream in asynchronously; as pieces pass Merkle validation,
  HarfBuzz and FreeType shape the arriving text into pages without dropping
  a single frame.

#### 3. Asymmetric Multi-Span Links ($N \\times M$) vs. Traditional Selection

- **Purist Position**: Web hyperlinks are broken: they are unidirectional,
  single-point to single-point, untyped, and decay when targets change.
  Xanadu links connect $N$ disjoint source spans to $M$ disjoint destination
  spans with explicit semantic typing (`Critique`, `Evidence`, `Illustration`).
- **Realist Constraint**: Standard desktop GUI conventions treat selection as
  destructive: clicking a second span clears the first. Users cannot juggle
  complex modifier sequences reliably without clear visual feedback.
- **Codebase Synthesis**: The **Link Forge** interface uses non-destructive
  accumulators backed by `ClickableRegistry`'s `tagKindGlyph`. Selected spans
  are held in left and right staging lists, rendered with persistent Electric
  Cyan brackets `⟦...⟧` on the left and Vivid Magenta brackets on the right,
  converging into a central hexagonal semantic type nexus.

______________________________________________________________________

## 6. Sovereign Document Genesis & Local Space Allocation

```
       +-------------------------------------------------------------+
       | Spatial HUD Dock                                            |
       |  [+ New Xanadoc]   [Open / Discover]   [Timeline]   [Beams] |
       +-------------------------------------------------------------+
                               │
               Click "+ New Xanadoc" / Ctrl+N
                               │
                               ▼
       +-------------------------------------------------------------+
       | 3D Canvas Void                                              |
       |                                                             |
       |     +-----------------------------------------+             |
       |     | * doc@v0:genesis (Local Sovereign)      |             |
       |     |-----------------------------------------|             |
       |     | |                                       |             |
       |     |   Caret blinking at byte offset 0.      |             |
       |     |   Typing streams directly into Slot 0   |             |
       |     |   UserPermascroll with 64 KiB stability |             |
       |     |                                         |             |
       |     +-----------------------------------------+             |
       |                                                             |
       +-------------------------------------------------------------+
```

### 6.1 Genesis Mechanics

- **Trigger**: Hotkey `Ctrl+N` or clicking `+ New Xanadoc` in the ambient
  spatial HUD dock.
- **Visual Settle**: A fresh document page quad ($600 \\times 800,\\text{px}$)
  smoothly scales up from the center of the camera frustum ($Z = 0$,
  $T = 0.18,\\text{s}$, cubic-bezier ease-out).
- **Identity & Provenance Header**:
  - The page top displays the author's local BEP 46 pubkey fingerprint badge
    and microversion tag `v0:genesis`.
  - The document is backed by a new `Store` instance with `MicroversionId{}`
    as its root.
- **Zero-Copy Permascroll Append**:
  - Keystrokes generate `OpKind::Insert` operations referencing slot 0
    (`LocalScroll`).
  - Text bytes append directly to the memory-mapped `UserPermascroll` file
    in 64 KiB page-aligned blocks, preserving BitTorrent v2 piece stability.

### 6.2 Local Document Open Palette

- Pressing `Ctrl+O` opens a spatial palette floating at $Z = 12,\\text{px}$
  in front of the active canvas:
  - Lists locally cached xanadocs (`.xudu` EDL files in the author's store
    directory).
  - Displays document title, creation timestamp, total microversions, and
    number of active transclusions.
  - Selecting a document smoothly glides any currently focused document into
    peripheral orbit and brings the chosen xanadoc into primary focus.

______________________________________________________________________

## 7. Distributed Document Discovery & BitTorrent Swarm Telescope

```
       +-------------------------------------------------------------------------+
       | Docuverse Telescope / Swarm Discovery Modal (Ctrl+O -> Swarm)            |
       | Search: [ #quantum-computing OR author:0x9f4a...                      ] |
       +-------------------------------------------------------------------------+
       | [Recent Local]        | [Followed Authors (BEP 46)] | [DHT Topic Swarms]|
       | - Philosophy_Notes    | - Ted Nelson (8 nodes)      | - #xanadu-core    |
       | - Xanadu_Spec_v2      | - Alan Kay (12 nodes)       | - #distributed-os |
       | - Media_Showcase      | - Douglas Engelbart (3)     | - #typography     |
       +-----------------------+-----------------------------+-------------------+
       | Selected: "The Foundations of Transclusion" by Ted Nelson               |
       | Swarm Health: 14 seeders | Merkle Verified: YES | Size: 1.4 MB         |
       | [Summon into 3D Space]                                                  |
       +-------------------------------------------------------------------------+
```

### 7.1 BEP 46 Mutable Author Catalogs

- Authors publish their portfolio using BEP 46 mutable torrent items addressed
  by their 32-byte Ed25519 public key (`bep46:<pubkey>/catalog`).
- The catalog payload is a tamper-proof JSON-LD index containing:
  - Document title, description, and canonical infohash.
  - Root `MicroversionId` and cryptographic Merkle root hash.
  - Hashcash PoW ticket preventing catalog spamming.
- In the UI, users can follow known author public keys or GPG fingerprints.
  The catalog updates quietly in the background via DHT polling.

### 7.2 DHT Topic Rendezvous Swarms

- For topic-based document discovery, topic names map deterministically to
  20-byte infohashes:
  $$\\text{Infohash}(\\text{topic}) = \\text{SHA-1}("xudu:topic:" + \\text{canonicalize}(\\text{topic}))$$
- Curators and readers announce themselves to the topic infohash on the
  Mainline DHT.
- Peer exchange allows clients to aggregate document announcements within topic
  swarms without centralized catalog servers.

### 7.3 Progressive 3D Streaming Materialization

- When an external document is summoned from the Swarm Telescope:
  1. A wireframe quad with animated edge glow appears in 3D world space at
     the target coordinate.
  1. `ManagedTorrent` downloads the 48-byte EDL descriptors and initiates
     swarm retrieval of the referenced scroll pieces.
  1. Arriving pieces pass through `merklecpp` validation against the published
     root.
  1. Once validated, text is immediately shaped via HarfBuzz and paginated;
     text quads fade in with a kinetic shimmer from top to bottom.

______________________________________________________________________

## 8. The Asymmetric Link Forge ($N \\times M$ Multi-Span Linking)

```
    Document A (Source)                                Document B (Target)
 +-------------------------+                        +-------------------------+
 | [...preceding text...]  |                        | [...introductory text...|
 |                         |                        |                         |
 | [Span A1: "premise 1"]  |----+              +--->| [Span B1: "counterarg"] |
 |                         |    |              |    |                         |
 | [...intermediate...]    |    |  +--------+  |    |                         |
 |                         |    +->| Link   |--+    |                         |
 | [Span A2: "premise 2"]  |------>| Forge  |------>| [Span B2: "rebuttal"]   |
 |                         |       | Nexus  |       |                         |
 +-------------------------+       +--------+       +-------------------------+
                                        ▲
                                        │
                             Select Type: [Critique]
```

### 8.1 Non-Destructive Span Accumulation

- **Selection**: Selecting text in any page renders a floating mini-badge
  `[+ Add to Left Anchor]` or `[+ Add to Right Anchor]`.
- **Margin Brackets**:
  - Left-side spans receive glowing **Electric Cyan brackets** `⟦...⟧` in
    the left document margin.
  - Right-side spans receive **Vivid Magenta brackets** `⟦...⟧` in the right
    document margin.
  - Up to 4 distinct overlapping link anchors per line are disambiguated by
    margin bracket depth and hue offsetting.
- **Accumulator Tray**: A compact floating bar docked between documents
  shows the active count: `Left: 2 spans | Right: 2 spans`. Individual spans
  can be clicked to inspect or dismissed with an `✕` badge.

### 8.2 The Semantic Type Nexus

- Docked between the two documents is the **Link Type Nexus**, offering
  first-class Nelsonian semantic link types:
  - 💬 `Comment / Annotation` (Cyan `#06B6D4`)
  - ⚖️ `Critique / Counterargument` (Crimson `#F43F5E`)
  - 🖼️ `Illustration / Evidence` (Purple `#A855F7`)
  - 📜 `Quotation / Attribution` (Identity Gold `#EAB308`)
  - 🌌 `Zigzag Dimension` (Emerald `#10B981`)
  - 🔄 `Revision / Alternative` (Amber `#F59E0B`)

### 8.3 Multi-Strand Connection Ribbons

- Clicking `[Forge Link]` (or pressing `Enter`) generates a persistent link
  record in the document's link collection.
- In 3D space, the link is rendered by `gleditor::Beams`:
  - The ribbon originates at each left span, merges into a bundle passing
    through the Nexus coordinate, and fans out smoothly to each right span.
  - Each strand is rendered with instance hue shifting to prevent ribbon
    occlusion and visual tangling.

______________________________________________________________________

## 9. Transclusion by Drag-to-Empty-Space

```
 Step 1: Grab Selection          Step 2: Drag into Void          Step 3: Settle & Link
 +--------------------+       +--------------------+          +---------+      +---------+
 | Select paragraph   |       | Source Page        |          | Source  |      | New Doc |
 | to transclude...   |       |  [Original Text]   |          | [Text]  |=====>| [Text]  |
 |                    |       |         \          |          +---------+      +---------+
 | [Transcluded Span] |       |          \ Elastic |               \                /
 |      (Drag)        |       |           \ Tether |                \ Identity Gold /
 +--------------------+       |            \       |                 \ Volumetric  /
                              |        +-----------+                  \   Prism   /
                              |        | Ethereal  |                   +---------+
                              |        | Page Quad |
                              |        +-----------+
```

### 9.1 Gestural Detachment

- **Interaction**: The user selects a span of text in any document, holds
  `Alt` (or drags the floating transclusion handle `⎘`), and pulls away from
  the parent document quad.
- **Blueprint Quad**: A semi-transparent ($40%\\text{ opacity}$) preview
  quad representing the new page/document detaches and follows the mouse cursor
  in 3D world space.

### 9.2 Elastic Tension Tether Physics

- During dragging, an animated, luminous quadratic Bezier spring tether connects
  the origin span to the dragged quad.
- The restoring force exerted on the tether is governed by Hooke's law with
  damping:
  $$\\mathbf{F}_{\\text{tether}} = -k_{\\text{tether}} (\\mathbf{P}_{\\text{cursor}} - \\mathbf{P}_{\\text{source}}) - c\_{\\text{tether}} \\mathbf{v}$$
- If the user releases within the minimum detachment threshold ($D < 120,\\text{px}$),
  the drag cancels and the quad snaps back into the source document with an
  elastic bounce.

### 9.3 Void Release & Kinetic Settle

- Releasing over empty space at distance $D \\ge 120,\\text{px}$:
  1. Spawns a new xanadoc or page quad at the release coordinates.
  1. The 3-Way Spring Solver engages, gliding the new document to a comfortable
     collinear reading position alongside the source
     ($X\_{\\text{new}} \\approx X\_{\\text{src}} + W + 60,\\text{px}$).
  1. Appends an EDL transclusion operation:
     $$\\text{OpKind::Transclude}(\\text{targetDoc}, \\text{offset}=0, \\text{sourceVersion}, \\text{spanStart}, \\text{spanLength})$$
  1. Instantiates an **Identity Gold volumetric transclusion prism** connecting
     the identical spans across 3D space. Both sides reference the exact same
     underlying primedia bytes in the author's scroll.

______________________________________________________________________

## 10. Intra-Document Page Spawning (`OpKind::PageBreak`)

```
      Page 1                                              Page 2 (Spawned)
 +----------------------------------+               +----------------------------------+
 | Paragraph 1 text...              |               | Paragraph 3 continues cleanly    |
 |                                  |               | on the new page...               |
 | Paragraph 2 text...              |               |                                  |
 |----------------------------------|               |                                  |
 | [ + Split to New Page (Ctrl+Ret) ]               |                                  |
 +----------------------------------+               +----------------------------------+
```

### 10.1 Inter-Paragraph Hover Gap Affordances

- When moving the mouse between paragraphs within a document, a subtle
  horizontal dashed accent line appears across the gap with a centered button:
  `[+ Split to New Page]`.
- Pressing `Ctrl+Enter` while editing immediately triggers the split at the
  current caret position.

### 10.2 Zero-Length Concatext Page Breaks

- The page split is recorded as an `OpKind::PageBreak` operation:
  - It does not insert newline characters or dummy spaces into the primedia
    scroll.
  - The layout engine (`text::TextLayout`) treats `OpKind::PageBreak` as an
    unconditional pagination boundary, advancing shaping to the subsequent
    page quad.
- The newly spawned page quad slides into position immediately to the right
  of the current page with a smooth horizontal gliding animation
  ($T = 0.28,\\text{s}$).
- Document page headers automatically update: `Page 1 of 2`, `Page 2 of 2`,
  with navigation buttons `[<]` and `[>]`.

______________________________________________________________________

## 11. Interactive Hypertime Branching DAG & Visual Diff Engine

```
 +------------------------------------------------------------------------------------+
 | Hypertime Branching Graph (Ctrl+H)                                                 |
 |                                                                                    |
 |  (v0:genesis) ────> (v1:draft) ────┬────> (v2:revisions) ────> (v3:head) [ACTIVE]   |
 |                                    │                                               |
 |                                    └────> (v1.1:alt-intro) ────> (v1.2:fork)       |
 |                                                                                    |
 | Scrub: [==================================O=============] 14:28:10 UTC             |
 +------------------------------------------------------------------------------------+
 | Side-by-Side Comparison: [v1:draft] vs [v3:head]                                   |
 | - Gold: Unchanged Primedia Spans (Transclusion Identity)                          |
 | - Mint: Inserted Spans (+42 chars)                                                 |
 | - Crimson: Deleted Spans (-18 chars to limbo)                                      |
 | [Quote Selected Span from v1 into Current Head]                                    |
 +------------------------------------------------------------------------------------+
```

### 11.1 2D Branching DAG Visualization

- Replaces the linear 1D version slider with a full 2D interactive DAG:
  - **Nodes**: Represent microversions ($v_0, v_1, v_2, \\dots$); node radii
    encode edit magnitude (number of concatext ops).
  - **Edges**: Represent edit operations leading from parent to child
    microversions.
  - **Forks**: Branch vertically along $+Y$; each distinct branch receives a
    unique branch lineage color.
  - **Status Badges**: The active working microversion is highlighted with an
    emerald pulse badge `[ACTIVE]`.

### 11.2 High-Precision Keystroke Scrubber

- A horizontal scrub bar at the base of the DAG allows scrubbing through time
  continuously:
  - Dragging the scrub thumb reconstructs document state at any microsecond in
    history.
  - Text quads dynamically update in place, demonstrating characters appearing,
    being deleted to limbo, or being transcluded.

### 11.3 Side-by-Side Comparative Diff Engine

- Clicking any node while holding `Shift` or clicking the `[Compare]` button
  places two historical versions side-by-side in 3D world space:
  - **Identity Gold Shading**: Spans that address identical underlying primedia
    addresses across both versions are shaded in soft gold.
  - **Vibrant Mint Shading**: Spans present only in the newer version
    (insertions).
  - **Subtle Crimson Shading**: Spans present in the older version but removed
    in the newer version (spans in limbo).
  - Note: This is not a fuzzy text diff (like `diff` or Myers diff); it is an
    exact, mathematical primedia address identity comparison.

### 11.4 Ancestral Span Transclusion

- Any text span in an ancestral or alternate branch version can be selected
  and immediately pulled into the active head document via the
  `[Quote this into Head]` quick action.
- This creates an instant transclusion operation, restoring deleted or
  historical text without duplicating characters or breaking address
  stability.

______________________________________________________________________

## 12. 3D Radial Marking Menu for Formatting & Alignment

```
                           [ N: Bold ]
                                │
          [ NW: Author Info ]   │   [ NE: Italic ]
                    \           │           /
                     \     +----+----+     /
                      \    |   TEXT  |    /
    [ W: Transclude ] ─────|  SELECT |───── [ E: Underline / Strike ]
                      /    +----+----+    \
                     /          │          \
                    /           │           \
          [ SW: New Link ]      │   [ SE: Alignment Sub-Wheel ]
                                │
                          [ S: New Page ]
```

### 12.1 Gesture Ergonomics & Fitts's Law

- Right-clicking or pressing `Space` while text is selected spawns an 8-way
  directional radial marking menu centered directly over the cursor.
- Radial menus exploit Fitts's law: target selection requires only a
  directional ballistic gesture ($O(1)$ spatial distance), allowing experienced
  users to execute formatting without visually looking at buttons:
  - **North (0°)**: Bold
  - **North-East (45°)**: Italic
  - **East (90°)**: Underline / Strikethrough
  - **South-East (135°)**: Alignment Sub-Wheel (fanning out Left, Center,
    Right, Justify)
  - **South (180°)**: New Page Break
  - **South-West (225°)**: Open Link Forge
  - **West (270°)**: Transclude Span to New Document
  - **North-West (315°)**: Inspect Author Provenance & Cryptographic Signature

### 12.2 Nelsonian First-Class Formatting Links

- In `xudu`, applying **Bold**, **Italic**, or **Justify** does **NOT** modify
  raw text bytes or inject markdown syntax (`**text**`, `<i>text</i>`).
- Instead, formatting is stored as a first-class `LinkType::Format` link:
  $$\\text{Link}(\\text{Type}=\\text{Format}, \\text{Left}=\\text{TargetSpan}, \\text{Right}=\\text{VocabSpan}(\\text{Format}::\\text{Bold}))$$
- **Transclusion Invariance**: Because formatting is attached to the primedia
  address via links rather than inline markup, a passage transcluded into three
  different xanadocs automatically retains its author formatting without syntax
  parsing errors.

______________________________________________________________________

## 13. Visual Spatial Architecture & Interface Layout

The spatial arrangement of all components inside `apps/xudu` is depicted below,
matching the reference rendering pipeline shown in `assets/xudu-spatial-ui.jpg`:

```
+------------------------------------------------------------------------------------+
|  [+ New Xanadoc] [Open / Swarm] [Hypertime DAG]              [Alice: 0x8F3A...]    |  <-- Ambient HUD
+------------------------------------------------------------------------------------+
|                                                                                    |
|   +--------------------+                         +--------------------+            |
|   | Hypertime DAG      |                         | Document B (Head)  |            |
|   | (Branching Tree)   |                         | (Z = 0, Active)    |            |
|   |                    |                         |                    |            |
|   |  (v0) -> (v1)      |       Volumetric        | ⟦ Right Anchor ⟧   |            |
|   |           \        |   Transclusion Prism    |                    |            |
|   |           (v1.1)   |========================>| ⟦ Transcluded ⟧    |            |
|   +--------------------+     (Identity Gold)     +--------------------+            |
|             \                                             |                        |
|              \ Elastic Bezier Tether                      | 3D Radial Marking      |
|               \                                           | Menu (Fitts's Law)     |
|                v                                          v                        |
|   +--------------------+                         +--------------------+            |
|   | Document A (v1)    |                         |   (N: Bold)        |            |
|   | (Z = -40, Tethered)|                         | (W:Trans) (E:Under)|            |
|   |                    |                         |   (S: Page)        |            |
|   +--------------------+                         +--------------------+            |
|                                                                                    |
|   [Scrub Slider: 18:32:04 UTC]                                [120 FPS / Vulkan]  |
+------------------------------------------------------------------------------------+
```

- **Render Layer Staging**:
  - Layer 0 ($Z = 0$): Active primary editing pages and text carets.
  - Layer 1 ($Z = 12,\\text{px}$): Floating HUD toolbars, Link Forge Nexus,
    and Radial Marking Menus.
  - Layer 2 ($Z = -20,\\text{px}$ to $-40,\\text{px}$): Collinear transcluded
    target documents and comparative diff pages.
  - Layer 3 ($Z = -60,\\text{px}$): Background parent documents connected via
    elastic quadratic Bezier tethers.
  - Layer 4 ($Z = -80,\\text{px}$): Hypertime branching tree and Swarm
    Constellation Browser.

______________________________________________________________________

## 14. Concrete Implementation Roadmap

The implementation of the missing UI elements is partitioned into seven
sequential stages:

| Stage | Feature Area | Key Classes & Source Files | Milestone Deliverable |
| :--- | :--- | :--- | :--- |
| **Stage 1** | **Spatial Genesis & Local Open** | `apps/xudu/session.cpp`, `apps/xudu/main.cpp` | Floating `+ New Xanadoc` HUD chip, `Ctrl+N` handler, and spatial local document open palette (`Ctrl+O`). |
| **Stage 2** | **3D Radial Marking Menu** | `include/gleditor/radial_menu.hpp`, `src/radial_menu.cpp` | 8-way directional radial menu registered with `ClickableRegistry` under `tagKindOverlay`, applying `LinkType::Format` and `TextAlign`. |
| **Stage 3** | **Interactive Hypertime DAG & Diff Visualizer** | `apps/xudu/hypertime_graph.hpp/.cpp`, `apps/xudu/store.cpp` | Replacing 1D `HypertimeMap` with 2D branching DAG, side-by-side comparative diffs, and keystroke scrub slider. |
| **Stage 4** | **Asymmetric Link Forge ($N \\times M$)** | `apps/xudu/link_forge.hpp/.cpp`, `apps/xudu/beams.cpp` | Left and right span accumulators with margin brackets `⟦...⟧`, type selection nexus, and multi-spine connection ribbons. |
| **Stage 5** | **Drag-to-Empty-Space Transclusion** | `apps/xudu/main.cpp`, `src/doc.cpp` | Kinetic drag tether spawning new pages or documents in void, with automatic `OpKind::Transclude` and volumetric prism rendering. |
| **Stage 6** | **Page Break Controls** | `src/text/layout.cpp`, `apps/xudu/session.cpp` | Inter-paragraph hover gap splitter and `Ctrl+Enter` shortcut inserting `OpKind::PageBreak` without primedia pollution. |
| **Stage 7** | **Decentralized Swarm Telescope** | `apps/xudu/core/managed_torrent.cpp`, `swarm_browser.hpp` | BEP 46 author catalog rendezvous, DHT topic swarms (`xudu:topic:<name>`), and 3D constellation browser. |

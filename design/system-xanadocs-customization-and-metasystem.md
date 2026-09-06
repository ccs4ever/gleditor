# System Xanadocs for Universal Customization & Metasystem State

## 1. Executive Summary & The Nelsonian Metasystem Principle

In classical computing environments, a rigid boundary separates "documents" (user data) from "configuration" (application preferences, keybindings, window coordinates, themes). Settings are relegated to ad-hoc text files (`.yaml`, `.json`, `.toml`), hidden dotfiles (`~/.config/`), or proprietary system registries. These configuration silos suffer from the exact pathologies Theodor Holm Nelson diagnosed in conventional computing:
- **No Hypertime History**: A typo or accidental override destroys previous settings. Reverting requires manual backups or deleting corrupted files.
- **No Character-Level Provenance**: Settings lack cryptographic authorship, timestamps, or commit ancestry.
- **No Universal Intertwingularity / Transclusion**: Sharing a keybinding profile or theme between machines or teammates requires out-of-band file copying rather than native transclusion.
- **Artificial Duality**: The artificial split between "data" and "code/preferences" is eliminated in Xanadu.

```
       +-------------------------------------------------------------------------+
       |                         User Permascroll (BEP 46)                       |
       +-------------------------------------------------------------------------+
             |                         |                         |
             v                         v                         v
     +---------------+         +---------------+         +---------------+
     |  User Xanadoc |         |  User Xanadoc |         | System Xanadoc|
     |   (Essay A)   |         |   (Notes B)   |         |  (system://)  |
     +---------------+         +---------------+         +---------------+
             |                         |                         |
       [Content Spans]           [Content Spans]         [Metasystem Spans]
             |                         |                         |
             |                         |                         +--> Keymap
             |                         |                         +--> Settings & Fonts
             |                         |                         +--> Notification Origin
             +-------------------------+-------------------------+--> Pouches / Drop Zones
```

In `gleditor` and `xudu`, **anything customizable is a System Xanadoc**.
A **System Xanadoc** is an append-only, content-addressed, microversioned document (`Store`) whose concatext and links encode metasystem state. Changing a keybinding, resizing UI text, repositioning notification toasts, or docking a drawer is not an ephemeral mutation of a struct in memory: it is an OSMIC operation (`OpKind::Insert` / `OpKind::Erase` / `OpKind::Transclude`) producing a new microversion in hypertime.

---

## 2. The Core System Xanadocs & Schemas

`xudu` organizes metasystem state into five canonical System Xanadocs:

```
+-------------------+-------------------------------------------------------------------+
| System Xanadoc    | Functional Purpose & Metasystem State Managed                     |
+-------------------+-------------------------------------------------------------------+
| system://keymap   | Command scancodes, modifier masks, action names, and help text.   |
| system://settings | UI font families, text sizes, document typography, color themes.  |
| system://layout   | Spatial 3D coordinates, notification spawn anchors, drawer docks. |
| system://ui       | Visibility toggles for HUD chips, beams, hypertime DAG, and tabs. |
| system://pouches  | Partitioned drop zones, user labels, colors, and ghost spanables. |
+-------------------+-------------------------------------------------------------------+
```

### 2.1 `system://keymap` (Command & Keyboard Actions)

Traditional keymap tables are statically compiled or read from static JSON. In `xudu`, the keymap is a live xanadoc.

#### Canonical Format
The concatext is human-readable, standard OSMIC declarative text:
```yaml
# Xudu Sovereign Keymap Specification
quit: Ctrl+Q # save and close
new-doc: Ctrl+N # create new sovereign document bound to permascroll
open-doc: Ctrl+O # spatial document open palette
save: Ctrl+S # save or preserve document
close: Ctrl+W # close active document
forward: Ctrl+Shift+N # step forward in hypertime history
scrub-forward: Ctrl+] # scrub forward along primary timeline
scrub-back: Ctrl+[ # scrub backward along primary timeline
transclude: Ctrl+T # transclude active selection
xanalink: Ctrl+L # mark anchor for xanalink
cancel-link: Ctrl+Shift+L # cancel pending xanalink
beams: Ctrl+K # toggle visual link ribbons and transclusion prisms
sworph: Ctrl+Shift+K # toggle sworphing document transport
publish: Ctrl+Shift+S # seal and publish active version
history: Ctrl+P # dump hypertime history
map: Ctrl+M # toggle hypertime DAG visualizer
radial-menu: Space # trigger 3D marking menu on selection
pouch-drawer: Ctrl+D # toggle spatial pouch drawer
```

#### Dynamic Hot-Reload
- `gleditor::CommandTable` is initialized from `system://keymap`.
- When an edit is made (either through a settings UI form or by editing the `system://keymap` document directly), `CommandTable::rebind()` parses the text and updates the event dispatch lookup table in $< 1\,\mu\text{s}$.
- Because the bindings are in a Xanadoc, a user who accidentally overrides an essential key can simply scrub backward (`Ctrl+[` or historical travel) to restore the previous keymap!

---

### 2.2 `system://settings` (Typography, Sizes & Color Themes)

Controls the typographic hierarchy and visual aesthetics across the entire rendering pipeline.

#### Canonical Format
```yaml
# Xudu Typographic & Theme Settings
ui.font.family: Sans
ui.font.size: 11
ui.font.tab_bar: Sans 10
ui.font.map: Sans 11
ui.font.radial_menu: Sans 12
doc.default_font: Sans 12
doc.line_height_multiplier: 1.25
render.theme: obsidian
theme.background: 0x14171DFF
theme.page_background: 0x1E222BFF
theme.text_normal: 0xE6EDF3FF
theme.transclusion_gold: 0xF59E0BFF
theme.link_cyan: 0x06B6D4FF
```

#### Render Thread Synchronization
- When `ui.font.size` or `ui.font.family` changes, `FontManager` resolves new `FontFace` instances and updates `DocumentSwitcher`, `FloatingToolbar3D`, `ToastOverlay`, and `HypertimeMap`.
- Text reflow is handled smoothly through `TextLayout` height-budgeted slicing without dropping frames.

---

### 2.3 `system://layout` (Spatial Geometry & Notification Coordinates)

Defines the physical layout of 3D document cosmos, floating overlays, and notification toasts.

#### Canonical Format
```yaml
# Spatial Coordinates & Toast Overlay Parameters
notifications.anchor: top-right # [top-right, top-left, bottom-right, bottom-left, top-center]
notifications.offset_x: 24.0
notifications.offset_y: 48.0
notifications.max_visible: 5
notifications.lifetime_seconds: 6.0
notifications.fade_duration_seconds: 0.25

document_row.spacing_x: 70.0
document_row.depth_step_z: -45.0
document_row.background_depth_z: -80.0

pouch_drawer.dock: right # [left, right]
pouch_drawer.width: 340.0
pouch_drawer.depth_z: 12.0
```

#### Dynamic Toast Positioning
In `ToastOverlay::draw()`, instead of hardcoded `marginX = 12.0F` and `marginY = 12.0F`, the overlay queries the active `LayoutConfig` derived from `system://layout`:
- **`top-right`**: $X = \text{screenWidth} - \text{width} - \text{offset}_X$, $Y = \text{screenHeight} - \text{offset}_Y - \text{stackHeight}$.
- **`bottom-right`**: $X = \text{screenWidth} - \text{width} - \text{offset}_X$, $Y = \text{offset}_Y$.
- **`top-left`**: $X = \text{offset}_X$, $Y = \text{screenHeight} - \text{offset}_Y - \text{stackHeight}$.
- **`bottom-left`**: $X = \text{offset}_X$, $Y = \text{offset}_Y$.

---

### 2.4 `system://ui` (Viewport Topology & Overlay Flags)

Maintains the user's active interface state and overlay visibility.

#### Canonical Format
```yaml
# Viewport Overlays & State
overlays.switcher_visible: true
overlays.map_visible: false
overlays.beams_visible: true
overlays.sworph_enabled: false
overlays.pouch_drawer_visible: false
overlays.radial_menu_enabled: true
```

---

### 2.5 `system://pouches` (Drop Zones & Transclusion Clasps)

As detailed in `design/xudu-pouch-drawer-and-clasp-bench.md`, backs the Pouch Drawer's drop zones:
- Drop zone labels (e.g. `"To Link"`, `"Notes for Later"`, `"Scratch"`).
- User-selected background tints for visual grouping.
- Ghost spanables transcluded into each zone with full Nelsonian character-level provenance.

---

## 3. Hypertime Mechanics for Metasystem State

### 3.1 The OSMIC Advantage for Configuration

| Capability | Classical Config Files (`.json`/`.yaml`) | System Xanadocs (`system://`) |
| :--- | :--- | :--- |
| **History & Undo** | None (overwritten in place) | Full OSMIC DAG (every microversion preserved) |
| **Branching** | Manual Git branch of dotfiles | Native hypertime branching (try experimental keymap) |
| **Recovery** | Delete file and hope for defaults | Step backward in hypertime (`session.scrubBackward`) |
| **Multi-Device Sync** | Third-party cloud sync / conflict files | BitTorrent v2 Merkle piece stability via BEP 46 |
| **Collaboration** | Complex merge conflicts | Zero-copy span transclusions and format links |
| **Provenance** | File modification timestamp | OpenPGP signature + author fingerprint |

### 3.2 Hypertime Scrubbing of Settings

Because a System Xanadoc is an instance of `xudu::Store`:
```cpp
// Scrub the keymap back 3 revisions to undo an erroneous binding:
auto &keymapStore = session.systemStore(SystemDocKind::Keymap);
auto history = keymapStore.allVersions();
if (history.size() >= 4) {
  auto targetVersion = history[history.size() - 4];
  session.repointSystemDoc(SystemDocKind::Keymap, targetVersion);
}
```

### 3.3 Author-Selectable Current Versions & Head Repointing

In classical version control (like Git), "HEAD" is a single pointer, while branches are named refs. In classical Xudu, `Store::latest()` fell back to the chronological tip with the greatest MicroversionId. However, an author often maintains **multiple parallel states considered current** (e.g. English Edition and French Edition, or Draft vs. Published Edition).

To reflect authentic Xanadulogical reality:
1. **Author-Selectable Set of Current Versions (`currentVersions`)**:
   - Every `xudu::Store` maintains an explicit, author-designated set of microversions considered active or current:
     $$\text{currentVersions} = \{v_1, v_2, \dots, v_k\} \subseteq \text{allVersions}$$
   - When a xanadoc is opened without specifying an exact microversion, all members of `currentVersions` are opened side-by-side as parallel active views.
   - Persisted beside the operations and primedia spools in `current.yaml`:
     ```yaml
     # Current active versions designated by the author
     current:
       - "1.4"
       - "1.2.1"
     ```

2. **System Xanadocs Constraint ($N = 1$ Active Head)**:
   - For all System Xanadocs (`system://keymap`, `system://settings`, `system://layout`, `system://ui`), the set of current versions is strictly constrained to **exactly one version**:
     $$|\text{currentVersions}| = 1$$
   - This single current version represents the **active live configuration** evaluated by the engine.

3. **Arbitrary Hypertime Repointing**:
   - The author can repoint a System Xanadoc's current version to **any past or alternate microversion in its history** at will:
     ```cpp
     store.repointCurrentVersion(targetMicroversion);
     ```
   - **Non-Destructive Time Travel**: Repointing changes the active head pointer without erasing downstream operations. If an author scrubs their keybindings back to state `1.1` to test a legacy profile, operations `1.2`, `1.3`, and branches `1.1.1` remain fully preserved in the operations spool. The author can repoint forward or branch into a new configuration at any time.
   - **Live Subsystem Notification**: When `repointCurrentVersion` is called on a system store, it triggers the registered `onCurrentVersionChanged` observer, instantly recompiling keybindings, recalculating toast layout vectors, or updating UI font descriptions without restarting the process.

### 3.4 First-Class Introspection: Opening & Editing System Xanadocs Live

While System Xanadocs operate headlessly behind the scenes by default to power the UI, they are **fully openable, inspectable, and editable as standard xanadocs**. In Nelsonian architecture, there are no black boxes.

1. **Zero Special-Cased Dialogs**:
   - Instead of sequestering configuration behind rigid graphical preference panes, an author can open any system xanadoc (e.g. `system://keymap`, `system://settings`, `system://layout`) directly into the document row via `Ctrl+O` or command line.
   - The document renders as a normal 3D quad at $Z = 0$, titled with its canonical URI (e.g. `⚙ system://keymap`).

2. **Interactive Live Typing & Hot Reload**:
   - The user can click anywhere in the system document and edit its text using standard caret movements, deletions, typing, and transclusions.
   - Every keystroke appends to the author's `UserPermascroll` and records an operation in the system store.
   - Because it is a System Xanadoc, the current version automatically advances with each edit ($|currentVersions| = 1$), and the engine hot-reloads the updated concatext immediately:
     - Changing `quit: Ctrl+Q` to `quit: Ctrl+X` in `system://keymap` dynamically updates `CommandTable` live while typing.
     - Tweaking `ui.font.size: 11` to `14` in `system://settings` instantly updates font faces across the interface.
     - Moving `notifications.offset_x` in `system://layout` shifts toast placement in real time.

3. **Hypertime Scrubbing as Live Config Time Travel**:
   - When viewing an open System Xanadoc, pressing `Ctrl+[` (`scrub-back`) or dragging the hypertime scrubber steps the document through past configurations.
   - Because the system xanadoc's active head is linked to its current view, stepping backward immediately repoints the active configuration to that historical state.
   - Stepping forward (`Ctrl+]`) returns to newer configurations without data loss.

---

## 4. Swarm Distribution & Transclusion of Presets

### 4.1 Sharing Keymaps & Presets as First-Class Scrolls

A user or community member can publish a specialized keymap or UI theme as a standard sealed xanadoc:
- Example: Alice publishes `vim_bindings.xanadoc` (salt: `vim-keys`).
- Bob wants Vim bindings in `xudu`. In Bob's `system://keymap`, rather than copying and pasting text, Bob **transcludes** the keymap from Alice's scroll:
  $$\text{transclude}(\text{dest}=\text{Bob's } system://keymap, \text{src}=\text{Alice's } vim\text{-keys})$$
- If Alice publishes an improvement to `vim-keys`, Bob's `xudu` automatically receives the update via BitTorrent swarm!
- If Bob wants to override a single key (e.g. keep `Ctrl+Q` for quit instead of `:q`), Bob inserts an override op into his local `system://keymap` branch. The transcluded base remains intact while Bob's local customization sits cleanly atop it.

---

## 5. C++23 Architecture & Class Structure

### 5.1 `SystemDocKind` & `SystemDocRegistry`

In `apps/xudu/session.hpp`:
```cpp
namespace xudu {

enum class SystemDocKind : std::uint8_t {
  Keymap,
  Settings,
  Layout,
  UI,
  Pouches,
  Count
};

constexpr std::string_view systemDocUri(SystemDocKind kind) {
  switch (kind) {
  case SystemDocKind::Keymap:   return "system://keymap";
  case SystemDocKind::Settings: return "system://settings";
  case SystemDocKind::Layout:   return "system://layout";
  case SystemDocKind::UI:       return "system://ui";
  case SystemDocKind::Pouches:  return "system://pouches";
  default:                      return "system://unknown";
  }
}

} // namespace xudu
```

### 5.2 Storage & Memory Model

- System Xanadocs reside on disk in `$XDG_DATA_HOME/xudu/system/<name>.xanadoc/` (e.g. `~/.local/share/xudu/system/keymap.xanadoc/`).
- If no system store exists on disk, `Session` automatically initializes one using the author's `UserPermascroll` and writes default canonical primedia and initial ops.
- System stores are loaded into `Session::stores` with `isSystem = true` so they do not appear in the normal document row unless explicitly inspected in a "Settings View".
- Access to active settings is cached in lightweight memory structures (`ParsedKeymap`, `LayoutConfig`, `ThemeConfig`) updated on every system store epoch change.

### 5.3 `Store` Current Versions API & Serialization

In `apps/xudu/core/store.hpp`:
```cpp
class Store : public SpanReader {
public:
  // -- Current Versions (Author-Designated Heads) --------------------------
  [[nodiscard]] const std::vector<MicroversionId> &currentVersions() const;
  [[nodiscard]] MicroversionId primaryCurrentVersion() const;
  void setCurrentVersions(std::vector<MicroversionId> versions);
  void repointCurrentVersion(const MicroversionId &version);
  void addCurrentVersion(const MicroversionId &version);
  void removeCurrentVersion(const MicroversionId &version);

private:
  std::vector<MicroversionId> currentVersions_;
};
```
- When `store.save(directory)` executes, it saves `current.yaml` listing `current: [v1, v2]`.
- When `store.load(directory)` executes, it reads `current.yaml`. If empty or unwritten, it falls back to `{latest()}`.
- For system stores, `repointCurrentVersion` validates that $|currentVersions| = 1$ and notifies `Session` of the active configuration change.

### 5.4 120 FPS Performance Envelope ($8.33\,\text{ms}$)

- Reading active layout offsets or keybindings during a frame is an $O(1)$ memory lookup.
- Metasystem ops are only parsed when a system store is edited, costing $< 50\,\mu\text{s}$.
- Zero allocations occur on the render thread during frame submission.

---

## 6. Integration Roadmap & Stage Alignment

The System Xanadoc paradigm seamlessly weaves through all stages of the UI overhaul:

1. **Stage 1: Spatial Genesis & Local Open (`Ctrl+N` / `Ctrl+O`)** *(Current Stage)*:
   - Introduce `SystemDocKind` foundation in `Session`.
   - Seed `system://keymap` with default bindings (`Ctrl+N` for `new-doc`, `Ctrl+O` for `open-doc`, `Ctrl+Shift+N` for `forward`).
   - Seed `system://settings` with initial UI text sizes ("Sans 10", "Sans 11").
   - Seed `system://layout` with notification spawn parameters.
2. **Stage 2: 3D Radial Marking Menu**:
   - Radial menu actions and slot assignments read from `system://settings` and `system://keymap`.
3. **Stage 3: Interactive Hypertime DAG**:
   - Allows visualizing and scrubbing hypertime for system xanadocs alongside normal user documents.
4. **Stage 4: Pouch Drawer & Clasp Bench**:
   - Directly backed by `system://pouches` as specified.
5. **Stage 5-7: Transclusion, Break Controls & Swarm Telescope**:
   - Transcluding community keymap and theme scrolls across the DHT swarm.

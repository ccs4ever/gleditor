# Native KJV beam prototype

The native prototype uses the full KJV, one stored three-member left endset, and a commentary
document with one right member. It opens Xuzz, not the SVG gallery. The source stores, author
permascroll, system documents and private activity store are isolated under
`build/kjv-link-prototype`.

## Prepare and open

```sh
make -j$(nproc) prototype/kjv-links
tools/run-kjv-link-prototype.sh
```

Preparation expects `tests/samples/kjv.txt`. If it is absent, provide your local KJV text to
`python3 tools/create-kjv-link-prototype.py --source /path/to/kjv.txt`. The input must contain verse
lines beginning with `Genesis 1:1`, `Psalm 63:5` and `Revelation 22:21`, followed by a tab. The
helper removes a UTF-8 BOM, imports the full text, creates a real stored link and records exact byte
locators in `prototype.json`. These prepared inputs demonstrate navigation; they do not prove UI
authorship of the fixture.

The launcher opens both stores through the Open dialog, explicitly chooses the first member and its
document occurrence on each side, and enters Genesis. The overview is hidden for this reading
composition; `Alt+Shift+V` shows it again. A wide window is useful for seeing both documents at
reading size. For a reproducible large viewport:

```sh
tools/run-kjv-link-prototype.sh --resize 1800,1050 --chord Alt+Shift+Return
```

The final Enter recomposes the chosen pages after resizing. Window resizing is ordinary platform
input; `--resize` is an automation hand, not a reader feature.

## Make the long jumps

The active side starts on Left. For each jump:

1. Press `Alt+Shift+J` to choose the next member.
1. Press `Alt+Shift+L` to choose its first occurrence, the KJV document.
1. Press `Alt+Shift+Return` to enter that exact passage.

The first jump reaches Psalm 63:5; the second reaches Revelation 22:21. With the current default
pagination they are on pages 623 and 1261 of 1261. Psalm is near the book's midpoint, rather than
the exact middle page. The intervening text remains unlinked. A registry cell also contains each
passage, so choosing the occurrence is deliberate; the prototype does not silently guess it.

`Alt+Shift+X` crosses the link while retaining the other side's cursor. `Alt+Shift+O` returns to
Genesis without replacing the selected member. Member, place, Cross, Enter, Origin and Back are also
named panel controls. `Alt+Shift+Escape` cancels a pending page entry. Selection previews do not
move the reading camera or append a completed visit.

## Reproduce the headless evidence

```sh
python3 tools/check-kjv-link-prototype.py --backend opengl \
  --out build/kjv-link-check-opengl
```

Use a fresh output directory for each run. The check copies the fixture and its isolated author
data, drives ordinary keyboard controls, captures first/middle/last/Origin frames and accessibility
output, checks exact native focus ranges, and compares both source stores before and after. Inspect
the pictures: focus logs and file integrity do not prove reading composition or input parity.

## Inspect one continuous member across a page break

Prepare a separate fixture that keeps the sparse link and adds a second, one-member link:

```sh
python3 tools/create-kjv-link-prototype.py --continuous \
  --root build/kjv-link-fragments-v2
KJV_PROTOTYPE_ROOT=$PWD/build/kjv-link-fragments-v2 \
  tools/run-kjv-link-prototype.sh --resize 1800,1050 --chord Alt+Shift+Return
```

From the initial sparse link, choose the next link with `Alt+Shift+N`, choose its left member and
document occurrence with `Alt+Shift+J` and `Alt+Shift+L`, then Cross, choose the notes occurrence,
and Cross back (`Alt+Shift+X`, `Alt+Shift+L`, `Alt+Shift+X`). Enter explicitly. The second link
remains `Left 1/1`; its range is Psalm 62:12 through Psalm 63:3. Current pagination puts the
continuous text on pages 622–623. Each page has its own bracket; open bracket ends indicate
continuation, and the physical page gap has no bracket or highlight. Scroll normally to inspect the
final-page fragment after the start leaves the viewport.

```sh
python3 tools/check-kjv-link-prototype.py --continuous \
  --root build/kjv-link-fragments-v2 --backend opengl \
  --out build/kjv-fragments-opengl-check
```

The extended check captures preview, entry and scrolling as well as the sparse jumps. It checks the
complete focus sequence, so choosing and scrolling cannot silently enter a different passage.
Changing scale may change pagination; the panel uses native layout rather than fixed page numbers.

## Current scope and remaining gates

The prototype has slim stationary satin connectors, crisp exact margin brackets, page-local visible
gatherings instead of a spine through disjoint gaps, explicit member and occurrence choices,
pending-page entry/cancellation, a note companion beside the entered passage, and real-page
locators. Completed transitions use the private activity store; source stores are not the activity
log.

Visible built pages now retain line-band geometry, allowing continuous members to draw exact local
fragments without reshaping text or requiring their start page to be built. The two-page Psalm case
is verified on OpenGL, OpenGL ES and Vulkan; this does not certify every multi-column/media case or
a continuous member spanning the entire book.

World drawing and picking now stay below the tab strip without changing the camera projection. The
extended checker accepts `--continuous --chrome`: after the clipped stop, an ordinary reverse wheel
reveals the complete selected continuation and context. Independent reader review accepts this
scoped scrolling fix on the captured native journey.

Selected-link previews now use a soft same-hue edge rather than a gold fill. The active exact
occurrence is strongest, the opposite chosen occurrence is quieter, and normal editing selection
remains blue. Text geometry stays on the page; actual lift awaits coordinated caret and picking
support. `system://ui` exposes the chosen and secondary edge widths.

Continuing margin anchors now reach the real paper edges: open at the page break, capped only at an
authored terminal. The physical gap remains visible. Their text highlights and connector attachments
stay exact; lane extents and visibility use the extended rails.

The broader revised proposal remains active work. Wide-page leaders with true picking pass-through,
named ambiguity recovery for crossings, honest margin overflow, bounded total dense-frame work and
the backend/scale matrix are not yet accepted. This sparse native KJV example is not a full J16 pass
or evidence that all dense arrangements are supported.

## Exact ranges follow-up

The final `build/kjv-fragments-{opengl,opengles,vulkan}-09/evidence/` captures show complete
endpoint byte boundaries without ambiguous trailing ellipses. Full optional location text remains
accessible. The focused component matrix also preserves all nine controls at 640×480 with doubled
type.

The separate narrow normal-type capture in `build/kjv-fragments-narrow-08/evidence/entry.png`
reveals an unresolved fallback: when no blank area fits the panel, it covers prose. See the native
prototype report for the evidence limits and remaining work; these captures do not certify full J16.

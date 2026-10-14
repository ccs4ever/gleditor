# Native UI font-scale and Link journey

The frozen application (`13e0efc05161179adac8ebc5f1f71c7f5d9279bf865cf6a8e957219cb0bc0a08`) was
exercised through the visible **UI font scale** command on a fresh profile for each run. The command
opened the native one-field form; replacing its value with `1.5` applied the change live. A restart
of the same profile retained the setting. The setting changes application chrome scale; it does not
change document text size.

| Backend and size   | Captures                           | Result |
| ------------------ | ---------------------------------- | ------ |
| OpenGL, 800×600    | [frames](800-600-opengl-controls/) | Passed |
| OpenGL ES, 800×600 | [frames](800-600-opengles/)        | Passed |
| Vulkan, 800×600    | [frames](800-600-vulkan/)          | Passed |
| OpenGL, 1440×1000  | [frames](1440-1000-opengl/)        | Passed |

Each successful run includes ten lossless PNG captures, an internal accessibility-tree snapshot in
`session.log`, restart output, profile-store operation dump, source-store operation dumps
before/after navigation, activity operation dumps, argv/runtime manifest, and SHA-256 list. Raw PPM
frames and copied profile directories are archived outside the repository under
`/workspace/spatial-build/fifthscratch/raw/font-native/`. `matrix.txt` records branch and base HEAD
(`975d8b1b0fb2b08659aa3db04ed81071d0526a43`). The internal tree is the application's diagnostic
snapshot; this run does not claim native AT-SPI/AccessKit availability.

The live journey entered an 8-line, 654-byte source and a 23-byte target, selected the exact full
source range, created a Link, entered its source document, navigated to Origin and Back, and
inspected the folded Link. The visible Link tree reports left member bytes `0–654` and right member
bytes `0–23`. Both source-document operation dumps compare byte-for-byte identical before and after
navigation. The activity dump gains one completed child visit after explicit Enter to the source
document; the font edit and subsequent Origin/Back navigation add no completed visits. The UI
system-doc dump contains the typed `1.5` set-value operation; the same-profile restart used that
stored profile.

At 800×600, the selected source remains present and readable in the tree, while the Link panel takes
some of the available canvas. Inspecting the folded Link reports
`Investigation needs more free canvas · enlarge window or reduce UI text`; the 800×600 view cannot
fully expand that investigation at scale 1.5, and the visible status text is clipped after “enlarge
window or …”. The diagnostic tree retains the full message. At 1440×1000 the Origin control remains
directly visible, while at 800×600 it is behind the Link panel's visible Controls toggle. This
explains the different pointer steps in the runs.

The first 800×600 OpenGL attempt stopped after entering the source because it did not expand
Controls before clicking Origin; it is preserved in `800-600-opengl/` and excluded from the
successful matrix. The first wide OpenGL script attempt similarly assumed a Controls toggle that was
not present at that size; it is preserved in `1440-1000-opengl-direct-failed/`. The rerun uses the
directly visible Origin control and passed.

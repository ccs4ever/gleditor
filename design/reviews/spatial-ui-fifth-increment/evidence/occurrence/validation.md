# Bounded native Link selection and preview

Run date: 2026-10-14. This is a bounded UI check on the unchanged fourth-increment binary. It is not
a full J24 journey and does not cover repeated occurrences, unavailable targets, or successful
cancellation of a pending target.

The tested executable was `build/xuzz`, SHA-256
`52dadabe6cfee1e5994a5e2e297ea35b6e9e6eef150f7f68012e9ff3a949cdf7`. It ran in the native
`spatial-native-session` container on OpenGL at 1024×768 with fresh data, config, and cache roots:
`/workspace/spatial-build/run-GiShtM/{data,config,cache}`. Exact arguments, the keymap debug output,
and every internal `--dump-a11y` tree are in [`selected/session.log`](selected/session.log). The raw
PPM frames remain outside Git at `/workspace/spatial-build/fifth-occurrence-native/selected-raw/`;
lossless PNG copies and their SHA-256 values are in this directory.

## Result

| Step                             | Native input                                                                                           | Result                                                                                                                                                                                                               |
| -------------------------------- | ------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Create documents and one Link    | `Ctrl+N`, selection, `Ctrl+Alt+[`, repeat for the second document with `Ctrl+Alt+]`, then `Ctrl+Alt+L` | Created two sovereign documents and one comment Link by typing and selecting in the UI. The Link has two left members and one right member.                                                                          |
| Select and inspect               | `Alt+Shift+N`, then `Ctrl+Alt+I`                                                                       | Selected Link 21 and opened the Master Link Tablet. The frame and internal tree show the same Link ID, authority, both sides, and `Current reader visit 1 · inspection keeps reading place`.                         |
| Preview a member and cross sides | `Alt+Shift+J`, `Alt+Shift+X`                                                                           | The tablet exposes `Left 1/2` and `Right 1/1`; the selected document text remains the current reading place. Preview did not create a visit.                                                                         |
| Try next occurrence              | `Alt+Shift+L`                                                                                          | The chosen left member remains `place 1/1`. This authored member has one available occurrence, so this run cannot establish repeated-occurrence browsing.                                                            |
| Cancel entry at origin           | `Alt+Shift+Escape`                                                                                     | The current visit stayed at 1 and the before/after-preview activity dumps are identical. There was no pending endpoint entry, so this is not evidence that cancellation of an in-flight or unavailable target works. |
| Enter and return                 | `Alt+Shift+Return`, then `Alt+Shift+B`                                                                 | Explicit entry created visit 2. Activity Back restored visit 1 with Link 21 still selected.                                                                                                                          |
| Close attempt                    | `Ctrl+W`, then `Escape`                                                                                | Ctrl+W closed the focused empty/default tab immediately; no confirmation appeared for Escape to cancel. The Link tablet remained at visit 1. This does not test cancellation of closing either authored document.    |

Both authored documents' operation dumps are byte-identical from the snapshot after Link creation to
the final snapshot, including after preview, entry, Activity Back, and the close attempt. The
activity store dump after preview and the no-pending cancel is also byte-identical to its
pre-preview dump. The later activity dump contains visit 2; Back returns to existing visit 1. Exact
dumps are in [`selected/`](selected/), with `cmp` results recorded by the validation run as success.

The startup log says `accessibility: not in this build`, which means native AT-SPI was unavailable.
The application's internal accessibility tree was available and retained at each capture. No claim
is made about OS accessibility clients.

## Evidence files

| Frame                                                         | What it records                                             |
| ------------------------------------------------------------- | ----------------------------------------------------------- |
| [`00-link-selected.png`](selected/00-link-selected.png)       | Link 21 selected; two left members and one right member.    |
| [`02-inspect.png`](selected/02-inspect.png)                   | Master Link Tablet and visit 1 before movement.             |
| [`03-member-next.png`](selected/03-member-next.png)           | Left member preview, with the right reading place retained. |
| [`05-occurrence-next.png`](selected/05-occurrence-next.png)   | The selected member still reports `place 1/1`.              |
| [`06-cancel-at-origin.png`](selected/06-cancel-at-origin.png) | State after the cancel chord with no pending entry.         |
| [`07-enter.png`](selected/07-enter.png)                       | Explicit entry, visit 2.                                    |
| [`08-activity-back.png`](selected/08-activity-back.png)       | Activity Back at visit 1.                                   |
| [`09-close.png`](selected/09-close.png)                       | Empty/default tab closed; Link tablet remains at visit 1.   |

`01-controls.png`, `04-cross.png`, and `10-after-escape.png` retain the other captured states. The
initial unselected attempt is kept separately under [`initial-unselected/`](initial-unselected/); it
is diagnostic and is not counted as a pass.

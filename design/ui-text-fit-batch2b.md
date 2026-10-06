# UI lift: batch 2b — focus core

This batch follows the boxed Canvas work in batch 2. It adds a library-owned focus manager and
migrates the existing modal input adapters without moving application models into the library.

## Input contract

`ui::FocusManager` registers borrowed scopes with RAII handles. Scope owners must stop dispatch and
release registration before destruction. Opening sequence, rather than registration order, selects
an active modal. Closing nested scopes restores the previous pane or modal, including its IME
rectangle. A modal consumes unhandled keys, composed text and pointer input; background command
execution requires approval from both the global allowlist and the focused scope.

`OutsidePointer` supports blocking, dismissal, and dismissal followed by dispatch to the restored
scope. A scope that handles a press captures that pointer until release or cancellation. Window
focus loss clears captures, modifiers, pending document clicks, selection dragging and touch gesture
state. F6 and Shift+F6 cycle registered panes. The key vocabulary includes PageUp, PageDown, Space
and F1 through F12, with optional printable codepoints decoded at the SDL boundary.

The platform loop and synthetic input share dispatch. Renderer automation uses the same manager for
keys and composed text. GPU picking remains asynchronous: a modal pick is offered only to the
focused modal, and never continues to background observers or the document caret.

## Adapter migration

The existing `ModalInput` interface remains as a compatibility adapter. The composite registers its
children and selects by activation sequence. Xuzz registers the publication form, ZigZag editing,
quotation builder, swarm telescope, store manager, pouch drawer, hypertime graph and radial menu.
The clasp forge is embedded in the pouch drawer and uses its scope. The modal adapters explicitly
notify activation on opening, so reopening an earlier registered modal gives it precedence.

`input.modalGlobalCommands` configures permitted background commands. Xuzz reads it live from
`system://keymap`; the plain editor reads its existing configuration format. Defaults allow only the
application quit command, which the compatibility adapter also approves. An empty list disables it.

Layout-based traversal and accessibility focus ownership belong to batch 3b. This batch preserves
existing field traversal and adds modal isolation around it.

## Verification

Focus manager unit tests cover opening order, restoration, command consent, outside-click policies,
pointer capture and cancellation, pane cycling, modifier reset and reentrant callbacks. Actual
binary orchestration opens each modal, sends composed text, Alt+Shift and document chords, Tab,
wheel and caret movement, then closes the modal and types a marker. Persisted operations and the
marker location detect background edits and lost caret restoration.

The final headless suite passed 613 library, 1,252 engine, 57 Xuzz and 119 ZigZag tests, followed by
12 swarm and one publication-outbox network test. GL/GLES/Vulkan backend comparison passed on an
isolated Xvfb display with software drivers; a captured Vulkan frame was inspected. Repository lint
and clang-format 19 checks of changed files passed. Repository-wide format-check remains blocked by
pre-existing drift in untouched files, including `hypertime_graph.cpp` and `edl_transform.cpp`.

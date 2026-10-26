# C++26 chainability audit

Audit scope: public `include/gleditor/` APIs and `apps/common/xanadu/` plus `apps/common/ui/`. This
is an inventory and migration proposal; no production code was changed. Repository guidance says
setters that only mutate return a pointer to the object, while a mutation that may refuse returns
`std::expected<T *, E>`.

## Existing convention

The convention is established rather than hypothetical:

- `ArenaManifold` uses `ArenaResult = std::expected<ArenaManifold *, ArenaRefusal>` and fluent
  `ArenaManifold *` setters/mutators such as `setDefaultSpanReader`, `bindDimension`, and
  `setExecutionLimits` (`apps/common/xanadu/zigzag/arena_manifold.hpp:202,263,387,410`). Mutators
  that can refuse use the expected alias. Its mark/release/discard API returns `ArenaManifold *` for
  successful state changes (lines 652,656).
- `QuotationBuilderOverlay` returns `QuotationBuilderOverlay *` from its setters and selection
  operations (`apps/common/ui/quotation_builder_overlay.hpp:125-156`); tests already exercise chains
  such as `setVisible(...)->setMode(...)` (`tests/zigzag/quotation_builder_overlay_test.cpp:136`).
- `StoreObjectManager` and `BranchConvergenceOverlay` also return their own pointer from setters;
  tests chain StoreObjectManager visibility (`apps/common/ui/store_object_manager.hpp:118-156`,
  `apps/common/ui/xanadoc/branch_convergence_overlay.hpp:90-107`,
  `tests/zigzag/store_object_manager_overlay_test.cpp:171`).
- `VPLEngine::withConsoleEffects` and `VplView::setDependencies` return their receiver, while nearby
  `VPLEngine::setStore` and several `VplView` property setters do not
  (`apps/common/xanadu/vpl/vpl_engine.hpp:45-48,82-83`, `apps/common/xanadu/vpl/view.hpp:93-115`).
- `Session::setAuthoredCommitHandler` is another chainable callback setter;
  `Session::referenceVisit` returns `std::expected<StoreActivityLog *, ActivityError>`
  (`apps/common/ui/xanadoc/session.hpp:464,610`).

This mixture gives a useful migration model: chain property/configuration setters, preserve
operation results, and use an expected pointer when refusal is part of the operation's contract.

## Recommended opportunities

Priority 1 means a local, value-only mutation with an obvious receiver and no virtual contract.
These are low semantic risk and support composition.

| Priority        | Current signature and location                                                                                                                  | Proposed signature                                                                                                                                                               | Evidence and constraints                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| --------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 3 (conditional) | `void Drawable::setModel(const glm::mat4&)` — `include/gleditor/drawable.hpp:14`                                                                | Do not change the base signature by itself. If fluent setup is justified, add derived wrappers returning `Doc *` and `Page *`, or separately justify a virtual/covariant design. | `Doc` and `Page` inherit `Drawable` (`include/gleditor/doc.hpp:159,373`), and the cited callers are derived (`apps/common/ui/xanadoc/views.cpp:2200,2217`; `src/doc.cpp:1394,1408`). Returning `Drawable *` would lose derived-member access on a chain. A derived wrapper hides the base method for derived-typed callers, but callers through `Drawable *` still receive only the base type; changing virtual dispatch is a larger API/ABI decision. Keep the current method until that cost is justified. |
| 1               | `void Doc::setDocIndex(uint32_t)` — `include/gleditor/doc.hpp:939`                                                                              | `Doc *setDocIndex(...)`                                                                                                                                                          | Direct assignment to picking metadata. No override/callback contract. Renderer frequently updates a document index while preparing render state, so returning `this` enables setup composition without changing the update.                                                                                                                                                                                                                                                                                  |
| 1               | `void VPLEngine::setStore(Store*)` — `apps/common/xanadu/vpl/vpl_engine.hpp:82-83`                                                              | `VPLEngine *setStore(...) noexcept`                                                                                                                                              | Direct non-owning pointer assignment; pair with existing `withConsoleEffects` to make engine configuration consistent. Audit lifetime remains caller-owned and should be documented as today.                                                                                                                                                                                                                                                                                                                |
| 1               | `void VplView::setOrigin`, `setExtents`, `setRagged`, `enclose`, `setScalarPayload` — `apps/common/xanadu/vpl/view.hpp:101,113,124,130,137,141` | Return `VplView *`; preserve `noexcept` on `setOrigin`, `setExtents`, `setRagged`, `enclose`, and both `setScalarPayload` overloads.                                             | Each is a direct member update, next to `setDependencies` which already returns `VplView *`. `setScalarPayload` overloads retain their overloads; no error or result is currently conveyed. `enclose` sets both `isEnclosed_` and `enclosedView_`, so chainability must not split that invariant.                                                                                                                                                                                                            |
| 1               | `void ScreenOverlay::setModel`, `setVisible`, `setBounds`, `setActionHandler` — `include/gleditor/ui/overlay.hpp:22-27`                         | `ScreenOverlay *...`                                                                                                                                                             | These are public configuration operations, not overrides. Implementations must keep their existing lock/invalidation behavior and return only after the state transition is complete. This enables one retained overlay to be configured in a setup expression.                                                                                                                                                                                                                                              |
| 2               | `void LinkForgeWidget::setLinkType`, `setProminenceTier` — `apps/common/ui/xanadoc/clasp_link_forge.hpp:96-109`                                 | `LinkForgeWidget *...`                                                                                                                                                           | Both compare, assign, and call `changed()`. Standalone current call sites (`tests/zigzag/clasp_link_forge_overlay_test.cpp:229-230`) remain valid. Do not chain `dropLeft`/`dropRight` or `cycleType` automatically: they represent user actions and state transitions where a sequence can hide intent.                                                                                                                                                                                                     |
| 2               | `void LinkPanelOverlay::setCellHighlighter`, `setAnchorResolver`, `setFramingHandler` — `apps/common/ui/xanadoc/link_panel_overlay.hpp:65-73`   | `LinkPanelOverlay *...`                                                                                                                                                          | These move callback values into members and return no status. Good builder/configuration composition. Keep callback types unchanged; callbacks may be retained, so `function_ref` is not appropriate here.                                                                                                                                                                                                                                                                                                   |
| 2               | `void VortexHost::setAppActionDelegate`, `setConfig` — `apps/common/xanadu/vortex/vortex_host.hpp:137-150`                                      | `VortexHost *...`                                                                                                                                                                | Config assignment also calls `applyParallelConfig()`. Return only after rebuilding the worker pool completes. This is configuration composition rather than an action. Do not chain `stepScheduler`, which has a meaningful count result.                                                                                                                                                                                                                                                                    |

Priority 1/2 setters can be converted with existing call sites unchanged because discarding a
non-`[[nodiscard]]` pointer result is valid C++. New call sites can compose setup:
`engine.setStore(&store)->withConsoleEffects(true)` or
`view.setOrigin(origin)->setExtents(extents)->setRagged(true)`. Since the project explicitly uses
pointer chaining, return `this`, not a reference.

## Refusal-capable APIs and non-candidates

Do not change a method's result to a pointer if it already reports a useful value, nor wrap an
existing value result just to create a chain. `ArenaManifold` is the model for refusal: its
`ArenaResult` conveys a refusal reason and a successful receiver pointer. Search should focus on
void mutators with an explicit refusal path (validation, resource allocation, authorization); those
should return `std::expected<T *, E>` only if the error can be represented without losing detail.
None of the selected priority candidates refuses.

Exclude these broad classes from this migration:

- Virtual overrides and interface callbacks: e.g. `FrameContributor::drawFrame`,
  `PickObserver::picked` (which returns a meaningful bool), and the SDL/render device extension
  setters. Their signatures are contracts, and changing them would require coordinated interface and
  backend changes.
- Commands and actions: `Doc::insert`, `Media::pause/stop/seek`, `Views::back`,
  `StoreObjectManager::createSlice`, `LinkForgeWidget::dropLeft`, and overlay `toggle`/navigation
  methods. Chaining these can obscure ordered effects or create false expectations of transactional
  composition.
- Lifecycle, asynchronous, synchronization, and output methods: `commit`, `draw`, `update`, `run`,
  `poll`, `synchronize`, `detach`, serializers, and worker callbacks. Their void return says the
  action is fire-and-forget or intentionally has no compositional result.
- Internal implementation helpers, constructors/builders, and callback signatures. Inventory matches
  include these and are not public API proposals.

## Functional composition beyond setters

The current APIs already provide useful composition tools; improvements should make their borrow and
error rules explicit rather than adding combinators indiscriminately.

- **`function_ref` is a borrow.** It is appropriate for a callback invoked during the call and never
  retained. `Driver::every` documents and follows that rule
  (`apps/common/xanadu/vicon/driver.hpp:45-48`; its implementation invokes the sink in the loop at
  `driver.cpp:55-56`). The render worker pool also borrows a callback only until `run()` returns,
  although workers invoke it concurrently before the call completes
  (`include/gleditor/render/worker_pool.hpp:64-65,80-84`). This means the callable must outlive the
  entire call, including all worker invocations, and captured mutable state may need
  synchronization. Never store a `function_ref` in an object, queue it for later, or let a callback
  escape. In contrast, setters such as `LinkPanelOverlay::setCellHighlighter` retain a callback, so
  their `std::function` ownership is intentional
  (`apps/common/ui/xanadoc/link_panel_overlay.hpp:65-73`).
- **Use `expected::and_then` for dependent fallible steps that share an error type.** The engine
  already uses monadic composition in font loading (`src/text/font.cpp:205,269`).
  `StoreActivityLog::reference` and `annotate` both return
  `std::expected<StoreActivityLog *, ActivityError>`, forwarded by `LinkContext::referenceVisit` and
  `annotateVisit` (`apps/common/xanadu/store_activity_log.hpp:44-46`,
  `apps/common/ui/xanadoc/link_context.hpp:98-104`). A caller that explicitly wants “annotate only
  if reference succeeds” can express that dependency with `and_then`; keep separate statements when
  the actions are independent or both outcomes matter. Do not nest `expected` results or discard the
  first error just to shorten control flow.
- **Keep optional references short-lived.** `LinkNavigator::selection()` and `ActivityLog::find()`
  expose `optional<T&>` to avoid copying stored records
  (`apps/common/xanadu/link_navigation.hpp:376`, `apps/common/xanadu/store_activity_log.hpp:35`).
  They compose with `transform`, `and_then`, or optional-as-range for immediate inspection, but the
  reference remains tied to the owning navigator/log and may be invalidated by a subsequent
  mutation. In particular, do not retain a selection reference across `dispatch`, `supply`, or any
  operation that can replace the selection. `ranges.hpp` explicitly constrains `findRef` to lvalue
  ranges for the same reason (`include/gleditor/ranges.hpp:43-50`).
- **Facade coverage already tests observable decisions.** Navigation tests exercise selection,
  resolution, entering and error outcomes through the `LinkNavigator` facade
  (`tests/xuzz/link_navigation_test.cpp:87,117,262-264`); reader-package UI tests cover reference
  and failed entry (`tests/zigzag/reader_link_packages_ui_test.cpp:387-389`). Any future refactoring
  to monadic composition should preserve those success/error outcomes and must not move the
  mutations across the facade boundary.

## API and binary compatibility

Changing a public return type is certainly a source API and function-type change: code taking a
member-function pointer, using a dependent expression, or compiling against the declaration observes
the new type. Binary impact is ABI- and call-direction-dependent, so this is not categorically an
ABI break. On common ABIs, the return type is absent from the exported symbol name and an old caller
that ignores the new pointer return may continue to call the same symbol. A newly compiled caller
expecting the returned pointer cannot safely use an old implementation that still returns `void`;
other ABIs, calling conventions, LTO, or signature-sensitive tooling may also distinguish the
change. Treat mixed old/new objects as unsupported unless the project's target ABIs are checked.
Rebuild all dependents and do not claim compatibility from matching symbol names alone.

Returning `this` is safe for a live lvalue and supports immediate chaining. It can produce a
dangling pointer when the receiver is a temporary, for example
`auto *p = makeOverlay().setVisible(true);` if `makeOverlay()` returns an overlay by value. The
implementation keeps these methods unqualified to match the established `ArenaManifold`,
`QuotationBuilderOverlay`, and `StoreObjectManager` fluent APIs and preserve existing calls on
rvalues. An immediate chain on a temporary remains within that full-expression, but a stored pointer
must not outlive the receiver. Ref-qualifying with `&` would prevent storing such a result but would
also reject existing rvalue calls, so that is a separate source-compatibility change. Do not add
`[[nodiscard]]` to these pointer results unless standalone setter calls are also migrated, since
that would turn the intended legacy style into warnings.

Thread safety and effect order remain those of each current method. For locked overlays, the method
must release its lock before returning `this`; returning from within a scope holding a local lock
guard is safe because destruction of the guard occurs while returning, but chained calls start only
after it is released. For VortexHost configuration, keep worker-pool reconfiguration synchronous
with the current setter.

## Migration phases

1. **Freeze the target list.** Start with the priority 1 direct setters and add only methods whose
   full implementation is a simple mutation. Record overrides, virtual bases, callback typedefs, and
   meaningful return values as explicit exclusions.
1. **Convert the library leaf APIs.** Start with `Doc` and `ScreenOverlay`, returning their own
   pointer and preserving locking and dirty/revision updates exactly. Leave `Drawable::setModel`
   unchanged until derived return-type preservation for `Doc` and `Page` is designed. Update docs
   for lvalue-only lifetime if ref-qualification is selected.
1. **Convert engine/UI configuration APIs.** Align the VPL family with `setDependencies`, then
   convert selected callback/configuration setters. Keep refusal-capable methods as
   `expected<T *, E>` and retain richer successful results as-is.
1. **Adopt chains selectively.** Convert repeated setup blocks in callers to chains only where the
   chain reads as configuration. Keep action sequences as separate statements. Existing callers need
   not change to receive the API benefit.
1. **Verification.** Build library and all programs (`make -j$(nproc)`), run the existing test suite
   headlessly (`make -j$(nproc) test`) after API migration, and run format/lint gates. Add focused
   tests only where behavior changes beyond return values; there should be no behavioral change to
   test.

This audit itself was not built or tested; no production behavior changed.

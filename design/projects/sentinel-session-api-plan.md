# Session API absence migration

## Contracts

`Session` currently uses valid values as failure markers when a view index is invalid. A root
version is `MicroversionId{}`, and the first store is index zero, so callers cannot distinguish a
valid root view/store from an invalid index.

| API                                                      | Proposed return                               | Missing/error cases                                                                       |
| -------------------------------------------------------- | --------------------------------------------- | ----------------------------------------------------------------------------------------- |
| `insertText`, `insertMedia`, `insertBreak`, `insertSpan` | `std::expected<MicroversionId, SessionError>` | Invalid view index or unavailable selected authoring context                              |
| `transclude`                                             | `std::expected<MicroversionId, SessionError>` | Invalid destination/source view or unavailable destination context                        |
| `transcludeText`                                         | `std::expected<MicroversionId, SessionError>` | Invalid destination/source view, unavailable destination context, or query text not found |
| `addLink`                                                | `std::expected<MicroversionId, SessionError>` | Invalid view index                                                                        |
| `importBranch`                                           | `std::expected<MicroversionId, SessionError>` | Invalid or absent target store                                                            |
| `versionOf`                                              | `std::optional<MicroversionId>`               | `nullopt` only for an invalid view index; a valid root view returns an engaged zero ID    |
| `storeIndexOf`                                           | `std::optional<std::size_t>`                  | `nullopt` only for an invalid view index; store index zero remains engaged                |
| `authoringContextForView`                                | Existing `std::optional<MicroversionId>`      | `nullopt` means invalid/unavailable context; engaged zero means explicit root context     |

Use a small `SessionError` enum with distinct invalid destination, invalid source, unavailable focus
context, and source text not found cases. Errors from lower-level Store calls that already throw
remain exceptions. A successful no-op is still an engaged result: for example, inserting empty text
at root or transcluding an empty span set may return the existing root version.

## Caller ownership

- Root owns the `Session` declarations/definitions and `apps/xuzz/xuzz_app.cpp`.
- Systems owns shared UI callers in `apps/common/ui/xanadoc/`: `views.cpp`,
  `batch_orchestrator.cpp`, `beams.cpp`, and `clasp_link_forge.cpp`. The batch orchestrator also
  calls `importBranch`.
- The Session test owner updates `tests/zigzag/paired_edit_heads_test.cpp` and
  `tests/zigzag/session_media_cache_test.cpp`.
- Audit all `versionOf` and `storeIndexOf` uses before migration. Current references include shared
  UI, `xuzz_app.cpp`, and the tests above; use optional presence checks before indexing stores or
  building a text source.

## Regression gate

Cover valid root and nonroot views, an invalid index, explicit root authoring context, unavailable
focus context, store index zero, and an invalid store/view index. Verify an empty branch import is
still a successful engaged root result. Verify invalid authoring calls append no operations or
permascroll bytes. Verify successful empty edits and empty transclusions remain engaged results even
when their version is `MicroversionId{}`. At UI call sites, check the error before calling
`sourceFor`, indexing a store, or notifying authored-commit listeners.

# R5 disposition — vql-lens-bridge v3.8 -> v5.3

Codebase CONVERGED. The other five reported NOT CONVERGED; all findings ACCEPTED and applied (the
doc was also user-redirected to camelCase + typed generators mid-round):

- camelCase builtins (user directive): lensMount/lensNext/... + cellKey/cellEq/cellScalar — vlace
  parity, deliberate.
- Typed generators replace lens("uri") (user: URIs aren't xanalogical):
  fsLens/gitLens/sqlLens/logLens/journalLens/cfgLens/ unionLens + spec fns
  dir/sql/git/log/journal/cfg.
- Varargs lens("keyword",...) (user): the generic generator through a keyword->factory registry;
  *Lens helpers desugar; custom lenses register a keyword, no Kw* needed.
- *Lens helpers MUST be Kw* tokens (ergo+writer B1): plain Identifiers parse as
  Context+FunctionInvocation, never the StoreGeneratorCall anchor -> Q3 registers
  KwFsLens/KwGitLens/ KwSqlLens/KwLogLens/KwJournalLens/KwCfgLens/KwUnionLens.
- Registry concrete shape (architect M1, vortex M1): vector\<weak_ptr> membership + owning
  vector\<shared_ptr>; LensRepl::session\_ -> shared_ptr; close expires.
- Ctor-lifetime scoping (architect M2): VQLEngine{VortexCore&}/ {ArenaManifold&} take an injected
  registry or \*Lens refuses.
- union: verb boundary (multiple): key-level ops only; every scoped verb Errs; duplicate-ExternalKey
  write -> refuse-ambiguous (first-success applyContentChange vs last-emission display).
- sessionFor arena-scoped + core() accessor (purist M3).
- lens() dedupe = (keyword,args) canonical tuple (purist M1/M2).
- Minors: lensstub dedupe; evaluateValueExpr-direct routing; depth counter is the def/functor
  backstop; compilePathStep silent drop (Q5); zero-roots unreachable; flat-for-in=before-first-for;
  cellKey/cellScalar cardinality; builtin-name dispatch precedence; attr-cell no-key all ops;
  findInDocuments; RouterLens staleness; sessionMountScopeAt + file rows; cite fixes.

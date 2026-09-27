# REPL result handoff

VQuery, VProlog and VPL exchange results as ordinary slice stores. A result slice has ordered rows
on `d.result` off home. A row may have one `d.source` cell containing local source references such
as `/path/to/store#cell=42`. The row text and typed value are ordinary cell content and Structure
operations, so Xuzz and `xudu-dump` can inspect them. The source string is an audit trail within a
user's workspace; it does not mint a persistent cross-store link.

- VQuery `:save <new-store>` writes the last successful query's returned cells, including their
  source store and cell reference when the result is a foreign cell.
- VProlog `:open <store>` compiles each result row as a Prolog clause. `:save <new-file>` saves its
  live clauses as consultable Prolog source. `:export <new-store>` writes every solution to its last
  successful query, with references to the imported fact rows.
- VPL `:open <store>` binds the result rank to `data`. For other slices, `:dims` lists dimensions
  and `:rank <dimension>` binds one rank to `data`. `:save <new-store>` writes the current direct or
  scalar Vortex result, preserving typed numeric values and any imported row references.

All three REPLs use the author's persistent default permascroll, under `$XDG_DATA_HOME`, unless a
program was explicitly given another permascroll. A saved store still needs that permascroll to
render its text. Save commands require a new path so an earlier result is not overwritten.

## Native store streams

Batch `-o -` on VQuery, VProlog and VPL writes a ustar stream to standard output. Its only entries
are the unchanged `ops.nodes` and `store.tables` files. Query results and other human-readable
output go to standard error while the archive is written. A `-` input store reads that archive from
standard input: as a positional store for VQuery and VProlog, as `--store -` for VPL, and as the
positional store for Xuzz. VProlog's `-o -` needs a batch query (`-e` or `-q`) and cannot enter its
REPL.

For example, with one permascroll shared by the pipeline:

```sh
vquery --permascroll "$scroll" -e "$vql" -o - "$source_store" |
  vprolog --permascroll "$scroll" -e "$prolog_query" -o - - |
  vpl --permascroll "$scroll" --store - -e '# data' -o - |
  xuzz --permascroll "$scroll" -
```

Each program creates the directory given to `--permascroll` if needed. The archive carries no
primedia; the receiving program must use the same permascroll as the writer. Xuzz retains a streamed
store under the user's `xudu/xanadocs/stream-import-*` directory so edits to it survive restart.
References to rows of a streamed input use `store:<document-id>#cell=<ref>` because the temporary
unpacking directory is removed after the receiving process exits.

The result format records source addresses, but it does not yet track which individual Prolog facts
participated in a particular proof or which VPL inputs contributed to each computed scalar. A
VProlog solution points to the imported dataset rows; a calculated VPL scalar points to its open
input store. Exact dependency edges require provenance from the logic solver and array evaluator.

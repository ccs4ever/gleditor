all

# Same two rules mdl's own "default" style excludes: not every fenced block
# here is one language (some interleave a command with its output or a
# comment), and a title page isn't required to open on a heading.
exclude_rule 'MD040' # Fenced code blocks should have a language specified
exclude_rule 'MD041' # First line in file should be a top level header

# mdformat's --wrap 100 (see the Makefile) reflows every prose paragraph to
# this same limit, so this rule only ever fires on content the formatter
# can't touch: code blocks and tables carry real commands and reference rows
# that cannot be wrapped without breaking them, so only prose is held to it.
rule 'MD013', :line_length => 100, :ignore_code_blocks => true, :tables => false

# Nested lists throughout the docs are indented two spaces, matching the
# codebase's IndentWidth rather than markdownlint's four-space default.
rule 'MD007', :indent => 2

# Several walkthroughs show a `$ command` shell prompt to set off a command
# from the prose around it even when no output follows on the next line;
# that is a deliberate narrative choice, not a missed convention.
exclude_rule 'MD014'

# design/ documents that enumerate several parallel protocol messages or
# system xanadocs repeat the same subsection template ("Purpose", "Message
# Formats", "Canonical Format", ...) once per item; allow_different_nesting
# stops MD024 flagging those same-level siblings under different parents
# while still catching a true accidental duplicate at the same nesting.
rule 'MD024', :allow_different_nesting => true

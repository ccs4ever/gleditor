all

# Same two rules mdl's own "default" style excludes: not every fenced block
# here is one language (some interleave a command with its output or a
# comment), and a title page isn't required to open on a heading.
exclude_rule 'MD040' # Fenced code blocks should have a language specified
exclude_rule 'MD041' # First line in file should be a top level header

# README.md and design/ narrate in prose that already hard-wraps near 80
# columns, same discipline as the C++ (ColumnLimit: 80 in .clang-format);
# code blocks and tables carry real commands and reference rows that cannot
# be wrapped without breaking them, so only prose is held to the limit.
rule 'MD013', :line_length => 100, :ignore_code_blocks => true, :tables => false

# Nested lists throughout the docs are indented two spaces, matching the
# codebase's IndentWidth rather than markdownlint's four-space default.
rule 'MD007', :indent => 2

# Several walkthroughs show a `$ command` shell prompt to set off a command
# from the prose around it even when no output follows on the next line;
# that is a deliberate narrative choice, not a missed convention.
exclude_rule 'MD014'

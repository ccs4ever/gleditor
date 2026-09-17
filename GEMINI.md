@AGENTS.md

- This system is not in production yet, no need to preserve backwards compatibility of any format
  except when other constraints are involved such as alignment and cache line sizing.
- All key bindings across `xudu`, `zigzag`, and `xuzz` must be defined in the `system://keymap`
  system store, using Vortex function calls for their actions. The `gleditor` application is
  explicitly exempt: `apps/gleditor` is the plain text editor that must not share any code or
  dependency with Zigzag, Xanadu, or Xuzz, and retains its own independent YAML configuration.
- New C++ code must justify why it isn't being written in Vortex.
- New Vortex standard library code in Vortex must leverage existing Vortex standard library
  functions unless absolutely necessary.

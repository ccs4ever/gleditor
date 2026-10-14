# Third preservation journey (OpenGLES)

This journey is prepared for the frozen candidate `xuzz` SHA-256
`66089e427a2ab6876c31675cfe6e6c0818294c6ba156a9c264caa8ef3c5d7739` and `libgleditor.so.0` SHA-256
`419d2142fb3aa9f0181120fb084a05deae6a17935754eae61fe9cd662abf680d`. The runner validates both
hashes, uses fresh XDG directories, line-buffered logs, and waits 20 seconds after the link-preview
capture before taking the pre-Enter activity snapshot.

The forced fixture has 80 lines and 7,438 bytes, with Ctrl+Return at byte 3719. It sets UI font
scale to 1.5 through the native form, selects the full source, creates a 23-byte target, enters
through the visible Enter control, pans incrementally to P041+ on the later physical page, then
performs Back followed by Origin. The runner records source stores and activity before Enter, at
Enter, and after navigation. The prepared launch command uses X11 under Xvfb at 800×600. No native
run has been started; `launch.sh` requires the parent's `SIXTH_RUN_NATIVE_GO=GO` gate.

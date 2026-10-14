# VK native capture at the restart checkpoint

The guarded runner used the frozen candidate app
`66089e427a2ab6876c31675cfe6e6c0818294c6ba156a9c264caa8ef3c5d7739` and library
`419d2142fb3aa9f0181120fb084a05deae6a17935754eae61fe9cd662abf680d`. It completed with exit 0 under
fresh XDG/X11/Xvfb at 800×600. The UI font-scale form sets chrome scale 1.5; source input is 7438
bytes with Ctrl+Return at 3719, followed by visible Enter, incremental wheel continuation, direct
Back and Origin.

The lead's handoff audit verifies visit record counts 1 before Enter, 2 at Enter and 2 afterward,
and both source operation dumps byte-identical across those snapshots. All 12 PNGs match the
retained raw PPMs exactly. See `checkpoint-audit.json`, `argv.txt`, `session.log`, source/activity
dumps and the captured frames. Raw PPMs and private profiles remain outside Git.

Independent final visual review and complete journey acceptance remain pending for the local
session. This bounded capture does not complete J24/J25 or full J1–J32 validation.

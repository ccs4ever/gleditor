# Windows bundle

Run `build-msys2.sh` from an MSYS2 UCRT64 or MINGW64 shell after installing the native build
dependencies. The packaging workflow records the complete dependency list. LibVLC and SQLite use the
native MSYS2 `vlc` and `sqlite3` packages; the bundle includes LibVLC's codec plugins and their
recursively collected DLL dependencies.

MSYS2 does not provide the mandatory RNP OpenPGP library. `build-rnp.sh` builds the checksum-pinned
RNP 0.18.1 release with its bundled sexpp dependency and the supported OpenSSL backend, then
installs it into the selected MSYS2 prefix. Its upstream dependency build uses CMake with MSYS
Makefiles; gleditor continues to build with GNU Make. The bundle script calls this helper when
`pkg-config` cannot find `librnp`.

```sh
bash packaging/windows/build-rnp.sh build/windows/dependencies/rnp
ACCESSKIT_DIR=/path/to/accesskit-c-0.22.3 bash packaging/windows/build-msys2.sh
```

AccessKit is required for every distributable bundle. The job moves the original AccessKit resource
aside before running the bundled programs, then reads imported fixture text through Windows UI
Automation using `check-accessibility.ps1`. That client enumerates the test process's HWNDs,
including its hidden window, and checks `TextPattern.DocumentRange`; it does not move keyboard
focus.

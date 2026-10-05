# Native accessibility validation

The formula bundles AccessKit and resolves it beside `libgleditor`. The packaging job removes the
downloaded resource before starting the installed programs, so successful startup proves the runtime
survives outside its build directory.

The formula also fetches VideoLAN's pinned VLC 3.0.23 disk image for the native LibVLC SDK. Its
public headers, codec plugins, shared data, and framework dependencies ship with the runtime. The
SDK places plugins under `lib/vlc/plugins`, matching LibVLC's library-relative discovery, and
rewrites dynamic dependencies to `@loader_path`. The installed programs therefore use their bundled
runtime after the original SDK has been removed. SQLite comes from the Homebrew `sqlite` formula.

`check-accessibility.m` checks delivery through the macOS accessibility API. It starts its own Xuzz
process with a hidden Cocoa window, imports the fixture into an isolated temporary store, and reads
the document through `AXUIElement` attributes and text ranges. It never raises a window or changes
keyboard focus. The child is terminated after success or failure.

Build the checker at a stable path on a native macOS test runner:

```sh
mkdir -p build
"${CC:-cc}" -fobjc-arc -framework Foundation -framework ApplicationServices \
  packaging/macos/check-accessibility.m -o build/check-macos-accessibility
build/check-macos-accessibility --check-trust
```

TCC must already grant this checker Accessibility access. An untrusted checker exits with status 2
and identifies the missing permission; it does not open a permission dialog. Configure that
permission on the test runner before executing the readback:

```sh
DYLD_LIBRARY_PATH="$PWD/stage/usr/local/lib" \
  build/check-macos-accessibility stage/usr/local/bin/xuzz \
  tests/samples/quick_brown_fox.txt
```

The packaging job runs readback whenever its checker is trusted. Set the repository Actions variable
`GLEDITOR_REQUIRE_MACOS_A11Y=1` for a runner configured for native accessibility validation: missing
TCC permission then fails the job. A default untrusted hosted runner reports readback as unverified
while still checking compilation, runtime bundling, and startup.

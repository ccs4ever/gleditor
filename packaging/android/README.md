# gleditor on Android

An Android build of `gleditor` (not yet `xudu`, which additionally needs
libtorrent-rasterbar, Boost and OpenSSL cross-built for Android -- a large
enough second lift that it is left for a follow-up rather than folded into
this one). Two backends are carried, matching the desktop build:
**OpenGL ES 3.0**, which every device this targets has, and **Vulkan**,
which not all of them do -- see [Backends](#backends) for how the app
chooses between them.

## Why this looks the way it does

- **SDL's own Java, unmodified.** This project writes no Java or Kotlin.
  `org.libsdl.app.SDLActivity` (from `thirdparty/SDL/android-project`, a git
  submodule) is a stock Activity that loads native libraries named `SDL3`
  and `main` and forwards lifecycle and input events to them; every line of
  actual behaviour is C++ this repository owns, compiled into `libmain.so`.
  `apps/gleditor/main.cpp` is still the entry point -- `SDL_main.h`'s macro
  redirection is what lets SDL's JNI glue call the same `main()` the desktop
  binary uses.
- **vcpkg for text stack dependencies.** FreeType, HarfBuzz, FriBidi,
  libunibreak, Fontconfig, and GLM are built via vcpkg's `arm64-android` and
  `x64-android` triplets from source, and both default to static linkage, so
  none of it needs a separate packaging step to end up in the APK -- it links
  straight into `libmain.so`. See `vcpkg.json` for the manifest and
  `/CMakeLists.txt` for how the toolchain is chained under the NDK's own.
- **Two ABIs, two product flavors.** `arm64-v8a` for real devices, `x86_64`
  for the classic Android Studio emulator image (Apple Silicon hosts default
  to `arm64-v8a` emulator images instead, which this also covers). Each
  needs a different `VCPKG_TARGET_TRIPLET`, which is why they are separate
  Gradle product flavors rather than one `abiFilters` list -- see the
  comment on `flavorDimensions` in `app/build.gradle`.
- **Assets extracted at startup, not read from the APK directly.**
  `shader_source.cpp` opens shader files with `std::ifstream`, which cannot
  read out of a zip. `src/android_bootstrap.cpp` copies the small fixed set
  of shader files (and a generated `fonts.conf` -- see below) out of the APK
  to the app's internal storage on every launch and points
  `GLEDITOR_ASSET_DIR` at them, which `paths.cpp` already honours verbatim
  on every platform.
- **A generated fontconfig.** Android has fonts but no fontconfig of its
  own -- font lookup there goes through a Java API this program does not
  use. `app/src/main/assets/fontconfig/fonts.conf` points fontconfig at
  `/system/fonts` (present on every Android build) and restates, as generic
  aliases, the `monospace`/`sans-serif`/`serif` mappings a desktop
  fontconfig install would otherwise supply from `/etc/fonts/conf.d`, enough
  to resolve this program's own default of `Monospace 16`
  (`--font`, `src/app.cpp`). Which concrete family names Android actually
  ships (`Roboto Mono` is assumed here) is the one part of this build that
  is a considered guess rather than something checked against a device --
  if text renders in the wrong face, or not at all, this file is where to
  look first.

## Backends

The app starts with `opengles` by default
(`GLEDITOR_DEFAULT_BACKEND` in `/CMakeLists.txt`, read by the same
`--backend` machinery `src/app.cpp` exposes on desktop) because every
device this build's `minSdk` reaches supports it, where Vulkan support
varies by GPU driver. Vulkan is still fully built in
(`GLEDITOR_ENABLE_VULKAN=1`) and can be exercised without a rebuild:

```sh
adb shell am start -n io.github.ccs4ever.gleditor/org.libsdl.app.SDLActivity \
  --es backend vulkan
```

`src/android_bootstrap.cpp` reads that `backend` launch-intent extra over
JNI (SDL's Activity is unmodified, so this is the only way in that does not
mean writing a custom one) and sets `GLEDITOR_BACKEND`, which
`defaultBackendName()` in `src/app.cpp` already checks on every platform.

### Vulkan validation

`device_vk.cpp` already looks for `VK_LAYER_KHRONOS_validation` at
`vkEnumerateInstanceLayerProperties` time and enables it whenever it finds
it -- the same thing that happens on desktop when
`vulkan-validationlayers` is installed. `fetch-validation-layers.sh`
downloads Khronos's prebuilt Android binaries of that layer into
`app/src/debug/jniLibs/<abi>/`, a source set `app/build.gradle` only merges
into **debug** builds, so a release build never becomes debuggable enough
to load it and never ships it. Run the script before a local Gradle build
if `app/src/debug/jniLibs` is empty; the CI workflow runs it automatically.

## Building

This was written and wired up in an environment with no Android SDK/NDK
installed, so the build was never exercised locally -- `.github/workflows/
android.yml` is what actually proved it out, building both ABIs and running
the result on a headless emulator (`-gpu swiftshader`) as part of every CI
run: the APK installs, `SDLActivity.onCreate()` reaches `SDL_main`, and the
process is still alive 15 seconds after launch. To build locally, with
Android Studio (which supplies its own SDK/NDK) or a standalone
`cmdline-tools` install:

```sh
export ANDROID_NDK_HOME=/path/to/ndk/27.3.13750724   # app/build.gradle's ndkVersion
export VCPKG_ROOT=/path/to/vcpkg                      # any recent checkout; vcpkg.json pins port versions itself
cd packaging/android
./fetch-validation-layers.sh                          # needs jq and unzip
./gradlew assembleDebug
```

The first configure is slow: vcpkg is compiling freetype, harfbuzz, fribidi,
libunibreak, fontconfig and glm from source, for both `arm64-v8a`
and `x86_64`. Set `VCPKG_BINARY_SOURCES` to a
[binary cache](https://learn.microsoft.com/en-us/vcpkg/users/binarycaching)
to avoid paying that twice; the CI workflow does this with
`actions/cache`.

Debug APKs land in
`app/build/outputs/apk/{arm64,x86_64}/debug/app-*-debug.apk` -- install
either with `adb install` or by dragging it onto a running emulator window.

## What is not here yet

- **xudu.** Needs `libtorrent-rasterbar`, Boost and OpenSSL cross-built for
  Android on top of everything here.
- **A launcher icon.** The manifest names none; Android shows a generic
  placeholder in its place. `logo.png` at the repository root is the source
  to derive one from.
- **An in-app file picker.** A fresh install with nothing shared or opened
  into it still starts on an empty document -- the same thing `gleditor`
  with no arguments does on desktop. What *is* wired up is the other
  direction: `android:intent-filter`s on `SDLActivity` (see
  `AndroidManifest.xml`) let another app hand gleditor a file, either by
  "Open with" (`ACTION_VIEW`) or "Share" (`ACTION_SEND`), which
  `src/android_bootstrap.cpp`'s `openDocumentFromIntent()` reads via JNI and
  opens the same way a command-line argument would on desktop. An in-app
  "Open..." *picker* -- browsing for a file from inside gleditor rather than
  being handed one -- is a different feature (Android's Storage Access
  Framework, `ACTION_OPEN_DOCUMENT`) and still needs the small amount of Java
  (a `registerForActivityResult` call) a stock `SDLActivity` does not provide
  on its own, which is why that half is left out rather than done halfway.
  Saving (`Ctrl+S`, `src/renderer.cpp`'s `Renderer::saveDoc()`) writes back
  through the same Uri a document was opened or shared in from --
  `android_bootstrap.cpp`'s `androidSaveDocument()` -- so the same gap
  applies there too: a document `gleditor` itself created with "new" has no
  picker to ask where to save it, on Android or on desktop.

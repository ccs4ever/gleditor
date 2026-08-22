class Gleditor < Formula
  desc "GPU-rendered document library, a text editor and a xanadoc editor"
  homepage "https://github.com/ccs4ever/gleditor"
  license "GPL-3.0-or-later"

  # No tagged release exists yet, so this is a head-only formula: install with
  # `brew install --HEAD`. Once a release ships, a stable `url`/`sha256` block
  # can sit alongside this one the way every other formula's does.
  head "https://github.com/ccs4ever/gleditor.git", branch: "main"

  # macOS ships GNU Make 3.81, the last release before the licence changed to
  # GPLv3, and this Makefile uses target-specific `private` variables, which
  # 3.81 cannot even parse -- not a missing feature at run time but a syntax
  # error before the build starts. Homebrew's own make is installed as gmake
  # rather than linked over the system one.
  depends_on "make" => :build
  depends_on "pkgconf" => :build
  # `gmake install` still generates a .dep file for every source the Makefile
  # knows about, tests included, and that generation needs gtest/gmock's
  # headers findable even though this build never runs a test binary. Other
  # platforms have those on the compiler's default search path already
  # (whatever their libgtest-dev / gtest-devel / gtest package installed);
  # Homebrew has no such ambient location.
  depends_on "boost"
  depends_on "fontconfig"
  depends_on "freetype"
  depends_on "fribidi"
  depends_on "glm"
  depends_on "harfbuzz"
  depends_on "libtorrent-rasterbar"
  depends_on "libunibreak"
  depends_on "lmdb"
  depends_on "openssl@3"
  depends_on "sdl3"
  depends_on "sdl3_image"

  # AccessKit is what reports the user interface to screen readers --
  # NSAccessibility here, via view subclassing rather than a system service,
  # since macOS has neither a bus like AT-SPI to speak to nor a window
  # procedure to subclass the way Windows does. It has no Homebrew formula of
  # its own: its C bindings are published as one prebuilt archive per platform
  # instead, so it is fetched as a resource and pointed at with ACCESSKIT_DIR
  # below, exactly what the README tells anybody building from source to do.
  #
  # Pinned rather than tracking the latest release: a build that changes what
  # it links because somebody else tagged something is not a build anybody
  # can reproduce. Kept in step with the version the Windows and macOS CI jobs
  # fetch.
  resource "accesskit-c" do
    url "https://github.com/AccessKit/accesskit-c/releases/download/0.22.3/accesskit-c-0.22.3.zip"
    sha256 "b652e380fb78efe6721ad892f15b2224f38f661c3fb20436ef4c5b3ce0fe8177"
  end

  # The Vulkan backend: vulkan-loader is the `vulkan.pc` this build's PKGS
  # list asks pkg-config for, molten-vk is the driver behind it -- a Vulkan
  # implementation on top of Metal, since macOS has no native Vulkan driver of
  # its own -- and glslang compiles the portable GLSL bodies under
  # assets/shaders/ to the SPIR-V the backend loads. vulkan-headers is not
  # named directly because vulkan-loader already depends on it.
  #
  # A driver reached through a compatibility layer rather than implementing
  # Vulkan natively is a "portability" driver in Vulkan's own terms, and the
  # loader excludes one from vkEnumeratePhysicalDevices unless the instance
  # opts in; DeviceVK does that itself once it sees the loader offer
  # VK_KHR_portability_enumeration, and separately enables
  # VK_KHR_portability_subset on the device once MoltenVK reports it, which
  # the spec requires whenever a device does. Neither check is specific to
  # this formula -- they run on every platform, and are a no-op wherever the
  # loader is not carrying a portability driver, which today means everywhere
  # but here.
  #
  # This has not been proven against a live MoltenVK device: this formula
  # builds and links the backend, and packaging.yml's macOS job installs and
  # runs `--version` against the result, the same as it has always done for
  # OpenGL, because whether a GitHub-hosted runner's WindowServer hands SDL a
  # real GPU context at all is a standing unknown this project's CI has never
  # resolved for either backend -- see the packaging table in the README.
  depends_on "glslang" => :build
  depends_on "molten-vk"
  depends_on "vulkan-loader"

  def install
    resource("accesskit-c").stage buildpath/"accesskit-c"

    # SDL3 is what the code is written against and Homebrew ships it, so this
    # does not fall back to SDL2 the way the Debian package does.
    #
    # GLEDITOR_ENABLE_A11Y=1 rather than leaving it unset: unset settles for
    # whatever pkg-config happens to find, which on a machine with no system
    # AccessKit would silently produce a formula that cannot be used with a
    # screen reader. The resource above is fetched precisely so that does not
    # happen.
    #
    # A head-only formula has no real version to report; matched to the
    # placeholder every other packaging target in this tree uses instead.
    system "gmake", "install",
           "-j#{ENV.make_jobs}",
           "prefix=#{prefix}",
           "GLEDITOR_SDL=3",
           "GLEDITOR_VERSION=0.1.0",
           "GLEDITOR_ENABLE_A11Y=1",
           "ACCESSKIT_DIR=#{buildpath}/accesskit-c",
           "GLEDITOR_ENABLE_VULKAN=1"
  end

  test do
    # Both return before SDL, GL or the DHT are touched, so this is a real
    # check of the installed binary's dynamic dependencies and nothing more.
    system bin/"gleditor", "--version"
    system bin/"xudu", "--version"
  end
end

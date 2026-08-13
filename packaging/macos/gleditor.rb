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
  depends_on "googletest" => :build
  depends_on "boost"
  depends_on "cairomm"
  depends_on "glibmm"
  depends_on "glm"
  depends_on "libtorrent-rasterbar"
  depends_on "openssl@3"
  depends_on "pangomm"
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

  # Vulkan alone is not wired in here: it would need MoltenVK plus a software
  # rasteriser for a headless bottle build, which has no macOS equivalent as
  # settled as lavapipe/llvmpipe on the other platforms. The OpenGL backend
  # this formula does build is not exercised either -- whether a Homebrew
  # bottle build has a WindowServer to hand SDL a real context is untested --
  # so that and the Vulkan backend are left to the Linux and Windows packaging
  # jobs. glslang is left off the build dependencies for the same reason as
  # Vulkan: it exists only to compile the Vulkan shaders.
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
           "ACCESSKIT_DIR=#{buildpath}/accesskit-c"
  end

  test do
    # Both return before SDL, GL or the DHT are touched, so this is a real
    # check of the installed binary's dynamic dependencies and nothing more.
    system bin/"gleditor", "--version"
    system bin/"xudu", "--version"
  end
end

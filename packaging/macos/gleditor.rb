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
  depends_on "boost"
  depends_on "cairomm"
  depends_on "glibmm"
  depends_on "glm"
  depends_on "libtorrent-rasterbar"
  depends_on "openssl@3"
  depends_on "pangomm"
  depends_on "sdl3"
  depends_on "sdl3_image"

  # Neither Vulkan nor AccessKit is wired in here. The Vulkan backend would
  # need MoltenVK plus a software rasteriser for a headless bottle build, and
  # nothing in this tree implements AccessKit's macOS adapter yet -- it is a
  # third, wholly separate API from the Windows and AT-SPI ones already
  # spoken here (NSAccessibility via view subclassing, not UI Automation or a
  # D-Bus service). The OpenGL backend and the accessibility reporting this
  # formula does not attempt are exercised by the Linux and Windows packaging
  # jobs instead. glslang is left off the build dependencies for the same
  # reason: it exists only to compile the Vulkan shaders.
  def install
    # SDL3 is what the code is written against and Homebrew ships it, so this
    # does not fall back to SDL2 the way the Debian package does.
    #
    # A head-only formula has no real version to report; matched to the
    # placeholder every other packaging target in this tree uses instead.
    system "gmake", "install",
           "prefix=#{prefix}",
           "GLEDITOR_SDL=3",
           "GLEDITOR_VERSION=0.1.0"
  end

  test do
    # Both return before SDL, GL or the DHT are touched, so this is a real
    # check of the installed binary's dynamic dependencies and nothing more.
    system bin/"gleditor", "--version"
    system bin/"xudu", "--version"
  end
end

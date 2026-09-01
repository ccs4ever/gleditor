{
  lib,
  stdenv,
  pkg-config,
  makeWrapper,
  freetype,
  harfbuzz,
  fribidi,
  libunibreak,
  fontconfig,
  poppler,
  file,
  sdl3,
  sdl3-image,
  libGL,
  glm,
  glslang,
  vulkan-headers,
  vulkan-loader,
  libtorrent-rasterbar,
  lmdb,
  # libtorrent-rasterbar's own public headers reach into boost/predef and
  # openssl/opensslv.h, but nixpkgs' derivation for it does not propagate
  # either as a build input of its own -- so a consumer that only links
  # against it, rather than building it, never sees them and hits "unknown
  # endian" and "OpenSSL too old" from headers that were never actually
  # looking at this build's compiler or its real OpenSSL at all. Named
  # outright so pkg-config and the include search path see the genuine
  # ones.
  boost,
  openssl,
  version ? "0.1.0",
}:

stdenv.mkDerivation {
  pname = "gleditor";
  inherit version;

  src =
    let
      # The build tree, minus everything a build does not read. Nix hashes the
      # source, so leaving build/ in means every local compile changes the
      # derivation and rebuilds the world.
      keep =
        name:
        let
          base = baseNameOf name;
        in
        _type: !(base == "build" || base == ".git" || base == "result" || lib.hasSuffix ".o" base);
    in
    lib.cleanSourceWith {
      filter = keep;
      src = ../..;
    };

  nativeBuildInputs = [
    pkg-config
    makeWrapper
    glslang
  ];

  buildInputs = [
    freetype
    harfbuzz
    fribidi
    libunibreak
    fontconfig
    poppler
    file
    sdl3
    sdl3-image
    libGL
    glm
    vulkan-headers
    # The loader, not just the headers: vulkan.pc belongs to it, and without
    # the .pc file pkg-config returns nothing for the whole package set.
    vulkan-loader
    libtorrent-rasterbar
    lmdb
    boost
    openssl
  ];

  # The Makefile's compiler default is clang++; stdenv supplies its own, and
  # letting make's built-in default through is what makes CXX come from the
  # environment here.
  makeFlags = [
    "GLEDITOR_SDL=3"
    "GLEDITOR_ENABLE_VULKAN=1"
    "GLEDITOR_VERSION=${version}"
    "prefix=${placeholder "out"}"
  ];

  buildFlags = [
    "lib"
    "gleditor"
    "xudu"
    "shaders"
  ];

  doCheck = false;

  # Nix's generic builder does not pass -j to make on its own; without this
  # every translation unit compiles one at a time regardless of how many
  # cores the build sandbox actually has. This is what makes it pass
  # -j$NIX_BUILD_CORES instead.
  enableParallelBuilding = true;

  # The GL and Vulkan entry points are resolved at run time through SDL, so
  # they are not in the binary's RPATH and the loaders have to be findable.
  # libGL comes from the driver on a NixOS system, hence only the Vulkan
  # loader being pinned here.
  postInstall = ''
    for program in gleditor xudu; do
      wrapProgram $out/bin/$program \
        --prefix LD_LIBRARY_PATH : ${
          lib.makeLibraryPath [
            vulkan-loader
            libGL
          ]
        }
    done
  '';

  meta = {
    description = "GPU-rendered document library with a text editor and a xanadoc editor";
    longDescription = ''
      GL Editor draws text on the GPU. Documents are laid out as pages in a 3D
      scene, shaped and rasterised with Pango and Cairo, and drawn as instanced
      quads sampled from a glyph atlas. One rendering pipeline serves three
      graphics APIs, chosen at run time.
    '';
    homepage = "https://github.com/ccs4ever/gleditor";
    license = lib.licenses.gpl3Plus;
    mainProgram = "gleditor";
    platforms = lib.platforms.linux;
  };
}

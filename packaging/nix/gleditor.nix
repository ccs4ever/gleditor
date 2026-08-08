{
  lib,
  stdenv,
  pkg-config,
  makeWrapper,
  # The ABI-suffixed attributes. The unsuffixed pangomm, cairomm and glibmm in
  # nixpkgs are the older parallel versions, and do not provide the .pc files
  # this build asks for.
  pangomm_2_48,
  cairomm_1_16,
  glibmm_2_68,
  sdl3,
  sdl3-image,
  libGL,
  glm,
  glslang,
  vulkan-headers,
  vulkan-loader,
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
        name: type:
        let
          base = baseNameOf name;
        in
        !(base == "build" || base == ".git" || base == "result" || lib.hasSuffix ".o" base);
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
    pangomm_2_48
    cairomm_1_16
    glibmm_2_68
    sdl3
    sdl3-image
    libGL
    glm
    vulkan-headers
    # The loader, not just the headers: vulkan.pc belongs to it, and without
    # the .pc file pkg-config returns nothing for the whole package set.
    vulkan-loader
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

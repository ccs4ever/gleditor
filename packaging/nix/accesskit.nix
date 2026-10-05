{
  rustPlatform,
  fetchurl,
  unzip,
  stdenv,
}:

rustPlatform.buildRustPackage {
  pname = "gleditor-accesskit-c";
  version = "0.22.3";
  src = fetchurl {
    url = "https://github.com/AccessKit/accesskit-c/releases/download/0.22.3/accesskit-c-0.22.3.zip";
    sha256 = "b652e380fb78efe6721ad892f15b2224f38f661c3fb20436ef4c5b3ce0fe8177";
  };
  nativeBuildInputs = [ unzip ];
  cargoLock.lockFile = ../accesskit/Cargo.lock;
  doCheck = false;
  installPhase = ''
    runHook preInstall
    architecture=${stdenv.hostPlatform.uname.processor}
    case "$architecture" in
      aarch64) architecture=arm64 ;;
      i?86) architecture=x86 ;;
    esac
    mkdir -p "$out/include" "$out/lib/linux/$architecture/static" "$out/share/licenses/accesskit"
    cp include/accesskit.h "$out/include/"
    cp target/${stdenv.hostPlatform.rust.rustcTarget}/release/libaccesskit.a "$out/lib/linux/$architecture/static/"
    cp LICENSE-MIT LICENSE-APACHE "$out/share/licenses/accesskit/"
    runHook postInstall
  '';
}

# RNP is private to this application; it must not satisfy another package's
# system-library dependency. Continue scanning its own system dependencies.
%global __provides_exclude_from ^%{_libdir}/gleditor/.*$
%global __requires_exclude ^librnp[.]so[.]0[(][)]([(]64bit[)])?$

Name:           gleditor
Version:        0.1.0
Release:        1%{?dist}
Summary:        GPU-rendered text editor with three graphics backends

License:        GPL-3.0-or-later AND Apache-2.0 AND BSD-2-Clause AND BSD-3-Clause AND MIT
URL:            https://github.com/ccs4ever/gleditor
Source0:        %{name}-%{version}.tar.gz
Source1:        https://github.com/AccessKit/accesskit-c/releases/download/0.22.3/accesskit-c-0.22.3.zip
Source2:        https://github.com/rnpgp/rnp/releases/download/v0.18.1/rnp-v0.18.1.tar.gz

BuildRequires:  gcc-c++ >= 13
BuildRequires:  make
BuildRequires:  cargo >= 1.85
BuildRequires:  rust >= 1.85
BuildRequires:  unzip
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  pkgconfig(harfbuzz)
BuildRequires:  pkgconfig(fribidi)
BuildRequires:  pkgconfig(libunibreak)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(spdlog)
BuildRequires:  pkgconfig(poppler-cpp)
BuildRequires:  pkgconfig(libvlc)
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  file-devel
BuildRequires:  SDL3-devel
BuildRequires:  SDL3_image-devel
BuildRequires:  pkgconfig(gl)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  glm-devel
BuildRequires:  openssl-devel
BuildRequires:  lmdb-devel
BuildRequires:  cmake
BuildRequires:  json-c-devel
BuildRequires:  bzip2-devel
BuildRequires:  zlib-devel
BuildRequires:  pkgconfig(libtorrent-rasterbar)
# libtorrent-rasterbar's own headers use boost/predef at compile time, and its
# -devel package's dependency on it is weak rather than hard -- so it is named
# outright rather than left to be pulled in.
BuildRequires:  boost-devel
BuildRequires:  glslang
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

Requires:       hicolor-icon-theme
Recommends:     dejavu-sans-fonts
# Not a hard requirement: the Vulkan backend is one of three, and the other two
# work without a loader present.
Recommends:     vulkan-loader

%description
GL Editor draws text on the GPU. Documents are laid out as pages in a 3D
scene, shaped with HarfBuzz, rasterised with FreeType, and drawn as instanced
quads sampled from a glyph atlas.

One rendering pipeline serves three graphics APIs. Everything above the
backend is written against a single device interface and names no graphics
API, so the same frame can be produced through OpenGL 3.3, OpenGL ES 3.0 or
Vulkan 1.0, chosen at run time with --backend.

The renderer draws only what is on screen: pages outside the view are culled,
and pages too far away for their glyphs to be legible are drawn as one solid
bar per line. The glyph atlas is mipmapped so minified text does not crawl,
and grows on demand rather than being sized for the worst case.

%package devel
Summary:        Headers for building on the gleditor library
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       pkgconfig(spdlog)

%description devel
The headers, the linker name and the pkg-config file needed to build a program
against libgleditor. The library draws documents on the GPU and names no
document format; xudu, shipped in the main package, is one program built on it.

%prep
%autosetup
echo 'b652e380fb78efe6721ad892f15b2224f38f661c3fb20436ef4c5b3ce0fe8177  %{SOURCE1}' | sha256sum -c -
echo '423c8e32e1e591462f759adf8441b1c44bca96d9f5daff13b82e81a79f18ecfd  %{SOURCE2}' | sha256sum -c -
mkdir -p build/rnp-source
tar -xf %{SOURCE2} -C build/rnp-source
mkdir -p build/accesskit-source
unzip -q %{SOURCE1} -d build/accesskit-source

%build
# SDL3 is what the code is written against and Fedora ships it, so unlike the
# Debian package this one does not fall back to SDL2. GLEDITOR_VERSION is
# passed because the tarball has no git history to describe.
%set_build_flags
# RNP is absent from Fedora's repositories; bundle its pinned release privately.
packaging/rnp/build.sh build/rnp-source/rnp-v0.18.1 build/rnp "$PWD/build/rnp-prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib
export PKG_CONFIG_PATH="$PWD/build/rnp-prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LDFLAGS="$LDFLAGS -Wl,-rpath,%{_libdir}/gleditor"
packaging/accesskit/build-linux.sh build/accesskit-source/accesskit-c-0.22.3 "$PWD/build/accesskit"
# Installed programs use the private RNP directory and normal system-library
# lookup; the in-tree $ORIGIN fallback is not an installed RPM search path.
%make_build \
    RPATH_FLAGS= \
    libdir=%{_libdir} \
    GLEDITOR_SDL=3 \
    GLEDITOR_ENABLE_VULKAN=1 \
    GLEDITOR_ENABLE_A11Y=1 ACCESSKIT_LINK=static ACCESSKIT_DIR="$PWD/build/accesskit" \
    GLEDITOR_VERSION=%{version} \
    lib gleditor xudu shaders

%install
%set_build_flags
export PKG_CONFIG_PATH="$PWD/build/rnp-prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LDFLAGS="$LDFLAGS -Wl,-rpath,%{_libdir}/gleditor"
%make_install \
    RPATH_FLAGS= \
    prefix=%{_prefix} \
    bindir=%{_bindir} \
    libdir=%{_libdir} \
    includedir=%{_includedir} \
    datadir=%{_datadir} \
    mandir=%{_mandir} \
    GLEDITOR_SDL=3 \
    GLEDITOR_ENABLE_VULKAN=1 \
    GLEDITOR_ENABLE_A11Y=1 ACCESSKIT_LINK=static ACCESSKIT_DIR="$PWD/build/accesskit" \
    GLEDITOR_VERSION=%{version}

install -d %{buildroot}%{_libdir}/gleditor
cp -a build/rnp-prefix/lib/librnp.so* %{buildroot}%{_libdir}/gleditor/

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop
appstream-util validate-relax --nonet \
    %{buildroot}%{_datadir}/metainfo/%{name}.metainfo.xml

%files
%license LICENSE
%license build/rnp-prefix/share/licenses/rnp/*
%license build/accesskit/LICENSE-MIT build/accesskit/LICENSE-APACHE
%doc README.md
%{_bindir}/gleditor
%{_bindir}/xudu
%{_bindir}/xuzz
%{_bindir}/zigzag
%{_libdir}/libgleditor.so.0
%{_libdir}/gleditor/
%{_datadir}/gleditor/
%{_datadir}/applications/gleditor.desktop
%{_datadir}/metainfo/gleditor.metainfo.xml
%{_datadir}/icons/hicolor/256x256/apps/gleditor.png
%{_mandir}/man1/gleditor.1*
%{_mandir}/man1/xudu.1*

%files devel
%{_includedir}/gleditor/
%{_libdir}/libgleditor.so
%{_libdir}/pkgconfig/gleditor.pc

%changelog
* Fri Aug 07 2026 ccs4ever <ccs4ever@gmail.com> - 0.1.0-1
- Initial package.

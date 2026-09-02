Name:           gleditor
Version:        0.1.0
Release:        1%{?dist}
Summary:        GPU-rendered text editor with three graphics backends

License:        GPL-3.0-or-later
URL:            https://github.com/ccs4ever/gleditor
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++ >= 13
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  pkgconfig(harfbuzz)
BuildRequires:  pkgconfig(fribidi)
BuildRequires:  pkgconfig(libunibreak)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(poppler-cpp)
BuildRequires:  file-devel
BuildRequires:  SDL3-devel
BuildRequires:  SDL3_image-devel
BuildRequires:  pkgconfig(gl)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  glm-devel
BuildRequires:  openssl-devel
BuildRequires:  lmdb-devel
BuildRequires:  rnp-devel
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

%description devel
The headers, the linker name and the pkg-config file needed to build a program
against libgleditor. The library draws documents on the GPU and names no
document format; xudu, shipped in the main package, is one program built on it.

%prep
%autosetup

%build
# SDL3 is what the code is written against and Fedora ships it, so unlike the
# Debian package this one does not fall back to SDL2. GLEDITOR_VERSION is
# passed because the tarball has no git history to describe.
%set_build_flags
%make_build \
    libdir=%{_libdir} \
    GLEDITOR_SDL=3 \
    GLEDITOR_ENABLE_VULKAN=1 \
    GLEDITOR_VERSION=%{version} \
    lib gleditor xudu shaders

%install
%make_install \
    prefix=%{_prefix} \
    bindir=%{_bindir} \
    libdir=%{_libdir} \
    includedir=%{_includedir} \
    datadir=%{_datadir} \
    mandir=%{_mandir} \
    GLEDITOR_SDL=3 \
    GLEDITOR_ENABLE_VULKAN=1 \
    GLEDITOR_VERSION=%{version}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop
appstream-util validate-relax --nonet \
    %{buildroot}%{_datadir}/metainfo/%{name}.metainfo.xml

%files
%license LICENSE
%doc README.md
%{_bindir}/gleditor
%{_bindir}/xudu
%{_libdir}/libgleditor.so.0
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

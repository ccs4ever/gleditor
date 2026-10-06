{
  lib,
  writeShellApplication,
  writeShellScriptBin,
  writeText,
  python3,
  dbus,
  at-spi2-core,
  gobject-introspection,
  glib,
  mesa,
  makeFontsConf,
  dejavu_fonts,
  xvfb-run,
  xdotool,
  coreutils,
  bash,
  gleditor,
}:
let
  sessionConfig = writeText "gleditor-validation-session.conf" ''
    <!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
      "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
    <busconfig>
      <type>session</type>
      <listen>unix:tmpdir=/tmp</listen>
      <auth>EXTERNAL</auth>
      <servicedir>${at-spi2-core}/share/dbus-1/services</servicedir>
      <policy context="default">
        <allow send_destination="*" eavesdrop="true"/>
        <allow eavesdrop="true"/>
        <allow own="*"/>
      </policy>
    </busconfig>
  '';
  privateBus = writeShellScriptBin "dbus-run-session" ''
    exec ${dbus}/bin/dbus-run-session --config-file=${sessionConfig} "$@"
  '';
  python = python3.withPackages (ps: [
    ps.pygobject3
    ps.pillow
  ]);
in
writeShellApplication {
  name = "check-gleditor-installed";
  runtimeInputs = [
    privateBus
    dbus
    python
    xvfb-run
    xdotool
    coreutils
    bash
  ];
  text = ''
    # Match the package's Nix libraries instead of using host Mesa or GI.
    export GI_TYPELIB_PATH=${
      lib.makeSearchPath "lib/girepository-1.0" [
        at-spi2-core
        gobject-introspection
        glib
      ]
    }
    export PATH=${at-spi2-core}/libexec:"$PATH"
    export FONTCONFIG_FILE=${makeFontsConf { fontDirectories = [ dejavu_fonts ]; }}
    export LD_LIBRARY_PATH=${mesa}/lib:"''${LD_LIBRARY_PATH:-}"
    export LIBGL_DRIVERS_PATH=${mesa}/lib/dri
    export __EGL_VENDOR_LIBRARY_FILENAMES=${mesa}/share/glvnd/egl_vendor.d/50_mesa.json
    export LIBGL_ALWAYS_SOFTWARE=1
    export SDL_AUDIODRIVER=dummy
    export XDG_DATA_DIRS=${at-spi2-core}/share
    ${bash}/bin/bash ${./check-installed.sh} ${gleditor} \
      ${../smoke-test.sh} ${../check-accessibility-linux.py} ${mesa} "$@"
  '';
}

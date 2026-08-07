{
  description = "GPU-rendered text editor with OpenGL, OpenGL ES and Vulkan backends";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        # There is no tag to describe inside the Nix sandbox -- and no .git at
        # all, since the source filter drops it -- so the version is handed in
        # rather than discovered.
        version = "0.1.0";
      in
      {
        packages = {
          gleditor = pkgs.callPackage ./packaging/nix/gleditor.nix { inherit version; };
          default = self.packages.${system}.gleditor;
        };

        apps.default = flake-utils.lib.mkApp {
          drv = self.packages.${system}.gleditor;
        };

        # The build dependencies, plus what the tests and the backend
        # comparison need. gtest carries gmock in nixpkgs -- there is no
        # separate attribute for it -- and mesa is what makes a headless run
        # possible.
        devShells.default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.gleditor ];
          packages = with pkgs; [
            clang-tools
            gtest
            doxygen
            xvfb-run
            mesa
            vulkan-loader
          ];
        };

        formatter = pkgs.nixfmt-rfc-style;
      }
    );
}

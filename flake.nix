{
  description = "scout: a keyboard-first, ranger-style file manager for Wayland, built on Qt 6";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  # Only the flake checks use it, to evaluate the home-manager module.
  inputs.home-manager = {
    url = "github:nix-community/home-manager";
    inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs =
    {
      self,
      nixpkgs,
      home-manager,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      commit = self.shortRev or self.dirtyShortRev or "unknown";
    in
    {
      overlays.default = final: prev: {
        scout = final.callPackage ./package.nix { inherit commit; };
      };

      homeManagerModules.default = import ./nix/hm-module.nix self;
      homeModules.default = self.homeManagerModules.default;

      packages = forAllSystems (pkgs: {
        scout = pkgs.callPackage ./package.nix { inherit commit; };
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.scout;
      });

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.scout}/bin/scout";
        };
      });

      checks = forAllSystems (
        pkgs:
        {
          scout = self.packages.${pkgs.stdenv.hostPlatform.system}.scout;
        }
        // import ./nix/checks.nix { inherit self home-manager pkgs; }
      );

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.scout ];
          # sway, grim, wtype, dbus and the portal run tools/harness.
          packages = with pkgs; [
            clang-tools
            dbus
            gdb
            (lib.getBin glib)
            grim
            sway
            wtype
          ];
          SCOUT_XDP = "${pkgs.xdg-desktop-portal}/libexec/xdg-desktop-portal";
          shellHook = ''
            export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtwayland}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtsvg}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtimageformats}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.kdePackages.kimageformats}/${pkgs.qt6.qtbase.qtPluginPrefix}"
          '';
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}

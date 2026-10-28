{
  description = "scout: a keyboard-first, ranger-style file manager for Wayland, built on Qt 6";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
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

      checks = forAllSystems (pkgs: {
        scout = self.packages.${pkgs.stdenv.hostPlatform.system}.scout;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.scout ];
          packages = with pkgs; [
            clang-tools
            gdb
            grim
            sway
            wtype
          ];
          shellHook = ''
            export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtwayland}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtsvg}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.qt6.qtimageformats}/${pkgs.qt6.qtbase.qtPluginPrefix}:${pkgs.kdePackages.kimageformats}/${pkgs.qt6.qtbase.qtPluginPrefix}"
          '';
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}

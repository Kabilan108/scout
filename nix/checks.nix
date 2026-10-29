# Flake checks for the home-manager module and the config checker.
{
  self,
  home-manager,
  pkgs,
}:

let
  inherit (pkgs) lib;
  scout = self.packages.${pkgs.stdenv.hostPlatform.system}.scout;
  home =
    hmPkgs:
    home-manager.lib.homeManagerConfiguration {
      pkgs = hmPkgs;
      modules = [
        self.homeManagerModules.default
        {
          home.username = "scout-test";
          home.homeDirectory = "/home/scout-test";
          home.stateVersion = "25.05";
          programs.scout = {
            enable = true;
            settings = {
              hidden = true;
              sort = "modified";
              font = "JetBrains Mono";
              font-size = 11.5;
              editor = "code --wait";
            };
            bookmarks = {
              w = "~/work";
              s = "~/My Stuff";
            };
            keybindings = {
              "<C-t>" = "terminal";
              "<C-e>" = "set editor 'nvim -p'";
              q = null;
            };
            extraConfig = ''
              map gx open
            '';
          };
        }
      ];
    };
  plain = (home pkgs).config;
  # An overlay that marks its scout as the file manager wins over this flake's package.
  overlaid =
    (home (
      pkgs.extend (
        final: prev: {
          scout = prev.hello // {
            scoutFileManager = true;
          };
        }
      )
    )).config;
  expected = pkgs.writeText "expected-scout.conf" (
    lib.concatStringsSep "\n" [
      "set editor 'code --wait'"
      "set font 'JetBrains Mono'"
      "set font-size 11.5"
      "set hidden true"
      "set sort modified"
      "map gs cd '~/My Stuff'"
      "map gw cd ~/work"
      "map <C-e> 'set editor '\\''nvim -p'\\'''"
      "map <C-t> terminal"
      "unmap q"
      "map gx open"
      ""
    ]
  );
in
{
  home-manager-module =
    # nixpkgs' own `scout` is a URL fuzzer; without the overlay the module must not pick it.
    assert plain.programs.scout.package.outPath == scout.outPath;
    assert overlaid.programs.scout.package.pname == "hello";
    assert lib.any (p: p.outPath == scout.outPath) plain.home.packages;
    pkgs.runCommand "scout-home-manager-module" { } ''
      diff -u ${expected} ${plain.xdg.configFile."scout/scout.conf".source}
      touch $out
    '';

  check-config-rejects-errors = pkgs.runCommand "scout-check-config-rejects-errors" { } ''
    export HOME=$(mktemp -d)
    printf 'map g cd ~\nbogus\n' > bad.conf
    if ${scout}/bin/scout --check-config bad.conf > report; then
      cat report
      echo "expected --check-config to fail"
      exit 1
    fi
    cat report
    grep -q 'line 1: map g is a prefix of other bindings' report
    grep -q 'line 2: unknown command bogus' report
    touch $out
  '';
}

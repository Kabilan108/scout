# programs.scout for home-manager. `self` is this flake.
self:
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.programs.scout;
  generator = import ./config.nix { inherit lib; };
  # nixpkgs has an unrelated `scout`; only take pkgs.scout if this flake's overlay put it there.
  overlayScout = (pkgs.scout or { }).scoutFileManager or false;
  configText = generator.toConfig { inherit (cfg) settings bookmarks keybindings extraConfig; };
  hasConfig = cfg.settings != { } || cfg.bookmarks != { } || cfg.keybindings != { } || cfg.extraConfig != "";
in
{
  options.programs.scout = {
    enable = lib.mkEnableOption "scout, a keyboard-first file manager";

    package = lib.mkOption {
      type = lib.types.package;
      default = if overlayScout then pkgs.scout else self.packages.${pkgs.stdenv.hostPlatform.system}.scout;
      defaultText = lib.literalExpression "pkgs.scout from scout's overlay, else scout.packages.\${system}.scout";
      description = "The scout package to install.";
    };

    settings = lib.mkOption {
      type =
        with lib.types;
        attrsOf (oneOf [
          bool
          int
          float
          str
        ]);
      default = { };
      example = {
        hidden = true;
        sort = "modified";
        font = "JetBrains Mono";
        font-size = 11;
        editor = "nvim";
      };
      description = "`set` options for scout.conf.";
    };

    bookmarks = lib.mkOption {
      type = lib.types.attrsOf lib.types.str;
      default = { };
      example = {
        w = "~/work";
        n = "~/notes";
      };
      description = "Each key becomes `g<key>`, going to the folder.";
    };

    keybindings = lib.mkOption {
      type = lib.types.attrsOf (lib.types.nullOr lib.types.str);
      default = { };
      example = {
        "<C-t>" = "terminal";
        q = null;
      };
      description = "Keys mapped to a command line; null removes a default binding.";
    };

    extraConfig = lib.mkOption {
      type = lib.types.lines;
      default = "";
      description = "Lines appended to scout.conf as they are.";
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];
    # Left alone when nothing is configured, so a hand-written scout.conf survives.
    xdg.configFile."scout/scout.conf" = lib.mkIf hasConfig {
      source = generator.validate {
        inherit pkgs;
        inherit (cfg) package;
        text = configText;
      };
    };
  };
}

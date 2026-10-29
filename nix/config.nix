# Writes scout.conf from Nix values and checks it with `scout --check-config`.
{ lib }:

let
  # Mirrors CommandRegistry::quote: bare words stay bare, anything else is
  # single-quoted with ' written as '\''.
  quote =
    s:
    if s != "" && builtins.match "[^[:space:]'\"\\\\]*" s != null then
      s
    else
      "'" + lib.replaceStrings [ "'" ] [ "'\\''" ] s + "'";

  value =
    v:
    if builtins.isBool v then
      (if v then "true" else "false")
    else if builtins.isInt v || builtins.isFloat v then
      builtins.toJSON v
    else
      quote v;
in
{
  inherit quote;

  toConfig =
    {
      settings ? { },
      bookmarks ? { },
      keybindings ? { },
      extraConfig ? "",
    }:
    lib.concatMapStrings (line: line + "\n") (
      lib.mapAttrsToList (name: v: "set ${name} ${value v}") settings
      ++ lib.mapAttrsToList (key: path: "map ${quote "g${key}"} cd ${quote path}") bookmarks
      ++ lib.mapAttrsToList (
        keys: command: if command == null then "unmap ${quote keys}" else "map ${quote keys} ${quote command}"
      ) keybindings
    )
    + extraConfig;

  # The config file itself, which only builds if scout finds no errors in it.
  validate =
    {
      pkgs,
      package,
      text,
    }:
    pkgs.runCommand "scout.conf"
      {
        nativeBuildInputs = [ package ];
        inherit text;
        passAsFile = [ "text" ];
      }
      ''
        export HOME=$(mktemp -d)
        export XDG_CONFIG_HOME=$HOME/.config XDG_STATE_HOME=$HOME/.local/state XDG_DATA_HOME=$HOME/.local/share
        export QT_QPA_PLATFORM=offscreen LANG=C.UTF-8
        export FONTCONFIG_FILE=${pkgs.makeFontsConf { fontDirectories = [ ]; }}
        cp "$textPath" scout.conf
        # The report goes last, where nix's short failure log still shows it.
        if ! scout --check-config scout.conf > report 2>&1; then
          echo "generated scout.conf:"
          cat -n scout.conf
          cat report
          exit 1
        fi
        cat report
        cp scout.conf "$out"
      '';
}

{
  lib,
  stdenv,
  cmake,
  ninja,
  makeWrapper,
  qt6,
  kdePackages,
  libarchive,
  poppler-utils,
  ffmpegthumbnailer,
  xdg-utils,
}:

let
  # Plugin directories are compiled into the binary instead of exported through
  # QT_PLUGIN_PATH, so apps launched from scout don't inherit our Qt plugins.
  pluginDirs = map (p: "${p}/${qt6.qtbase.qtPluginPrefix}") [
    qt6.qtbase
    qt6.qtwayland
    qt6.qtsvg
    qt6.qtimageformats
    kdePackages.kimageformats
  ];
in
stdenv.mkDerivation {
  pname = "scout";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./CMakeLists.txt
      ./src
      ./tests
      ./data
    ];
  };

  nativeBuildInputs = [
    cmake
    ninja
    makeWrapper
  ];

  buildInputs = [
    qt6.qtbase
    qt6.qtwayland
    qt6.qtsvg
    qt6.qtimageformats
    kdePackages.kimageformats
    kdePackages.syntax-highlighting
    libarchive
  ];

  dontWrapQtApps = true;

  cmakeFlags = [ (lib.cmakeFeature "SCOUT_PLUGIN_DIRS" (lib.concatStringsSep ";" pluginDirs)) ];

  doCheck = true;
  preCheck = ''
    export QT_QPA_PLATFORM=offscreen
    export HOME=$(mktemp -d)
    export XDG_DATA_HOME=$HOME/.local/share
  '';

  postFixup = ''
    wrapProgram $out/bin/scout --suffix PATH : ${
      lib.makeBinPath [
        poppler-utils
        ffmpegthumbnailer
        xdg-utils
      ]
    }
  '';

  meta = {
    description = "Keyboard-first, ranger-style file manager for Wayland";
    mainProgram = "scout";
    platforms = lib.platforms.linux;
    license = lib.licenses.mit;
  };
}

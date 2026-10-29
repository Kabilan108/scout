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
  # Shown by `scout --version` and in scout.log; the flake passes its revision.
  commit ? "unknown",
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
  version = "0.3.4";

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

  # Symbols and line tables stay in the binary, so crash backtraces in
  # scout.log and coredumpctl have function names. -g1 keeps it small.
  cmakeBuildType = "RelWithDebInfo";
  dontStrip = true;

  cmakeFlags = [
    (lib.cmakeFeature "SCOUT_PLUGIN_DIRS" (lib.concatStringsSep ";" pluginDirs))
    (lib.cmakeFeature "SCOUT_COMMIT" commit)
  ];
  # cmakeFlags would split the value at its spaces.
  preConfigure = ''
    cmakeFlagsArray+=("-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG")
  '';

  doCheck = true;
  preCheck = ''
    export QT_QPA_PLATFORM=offscreen
    export HOME=$(mktemp -d)
    export XDG_DATA_HOME=$HOME/.local/share
    export XDG_STATE_HOME=$HOME/.local/state
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

  # Tells the home-manager module this is the file manager, not nixpkgs' URL fuzzer of the same name.
  passthru.scoutFileManager = true;

  meta = {
    description = "Keyboard-first, ranger-style file manager for Wayland";
    mainProgram = "scout";
    platforms = lib.platforms.linux;
    license = lib.licenses.mit;
  };
}

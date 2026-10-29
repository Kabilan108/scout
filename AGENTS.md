# scout

A keyboard-first, ranger-style file manager: Qt 6 Widgets, C++20, CMake, Nix.
Tony uses it daily on niri in place of Nautilus. Through `~/dotfiles` it is
also his file chooser for other apps (an xdg-desktop-portal backend) and the
"Show in folder" target (`org.freedesktop.FileManager1`).

## Where things live

- `src/mainwindow.cpp`: most features. Commands (`registerCommands`), default
  keys (`bindDefaults`), the Ctrl-K palette, prompts, tabs, undo, bulk rename,
  file chooser mode.
- `src/fileops.cpp`: copy, move, trash, delete and restore tasks, plus the
  rename helpers. This is where user data can be lost; see the invariants.
- `src/service.cpp`: `scout --service`, owning FileManager1 and the FileChooser
  portal. D-Bus starts it on demand.
- `src/config.cpp`: `scout.conf` and `scout --check-config`. `src/state.cpp`:
  remembered zoom and tabs. `src/logging.cpp`: `scout.log` and the crash handler.
- `src/overlays.cpp`: the `card` helpers every popup is drawn with.
- `nix/hm-module.nix`: `programs.scout` for home-manager.
- `tools/harness/`: headless verification against the real binary.

## Checks

Run inside the dev shell and capture the exit status:

```sh
nix develop -c bash -c 'cmake -B build -G Ninja && cmake --build build && QT_QPA_PLATFORM=offscreen ./build/scout_tests' > /tmp/scout-test.log 2>&1; echo "exit=$?"
```

- Changes to object lifetimes, models, previews or file operations: also run the
  suite in an AddressSanitizer build (`build-asan`, configured with
  `-DCMAKE_BUILD_TYPE=Debug -DSCOUT_SANITIZE=ON`).
- `nix build .#scout` runs the suite in the Nix sandbox. `nix flake check` also
  evaluates the home-manager module and its config validation.
- Changes the user can see, or changes to the dialogs or `--service`: read
  `tools/harness/README.md` and run the matching script.

## Invariants

- File operations never replace or lose data. Renames go through
  `renameNoReplace`. Deletes walk directory fds opened with `O_NOFOLLOW`.
  Copies build in a hidden sibling and move into place on success. A
  cross-device move removes only the source entries it copied, unchanged since.
  Any change here gets a regression test and an independent review.
- Undo acts only on items whose device, inode and birth time still match what
  it recorded.
- A new `set` option goes in four places: `config::checkOption`, the palette's
  completion list in `updateCommandList`, the README, and a test.
  `programs.scout.settings` passes it through as is.
- Text colors come from `Palette` roles, and `textMeetsContrastInEveryTheme`
  holds each role to 4.5:1. A new role joins that test.
- Popups draw through the `card` helpers so they share one style.
- `defaultBindingsNeverShadowEachOther` keeps every default key reachable
  without a pause. Run it after changing `bindDefaults`.

## Tests stay inside their temp dirs

Tests point the XDG dirs at temporary folders, and the launcher hook in
`initTestCase` replaces real program launches. A test that navigates (`h`, `l`,
`cd`, find) or creates and deletes files works on fixed paths under its own
temp root. Random walks run in a child confined with Landlock, as
`visualModeSurvivesRandomUse` does. An unconfined stress test once climbed to
`/tmp` and deleted the user's files.

## Known traps

- KSyntaxHighlighting queues `rehighlightBlock` calls that hold blocks of the
  current text. Every text preview change goes through `Preview::replaceText`,
  which drops them first.
- When the compositor connection breaks, Qt's Wayland plugin calls `_exit`: no
  core dump, only a warning in `scout.log`.
- `scout --service` starts while xdg-desktop-portal waits for it. A blocking
  D-Bus call on that path deadlocks, so calls made at startup are asynchronous.
- Packaged builds ignore `QT_PLUGIN_PATH`, since plugins from another Qt
  version abort startup. Dev builds rely on the dev shell's.
- The dotfiles build scout through `overlays.default` against their own
  nixpkgs, so the user's Qt can differ from this flake's.
- nixpkgs has an unrelated package named `scout`. The home-manager module only
  takes `pkgs.scout` when it carries the `scoutFileManager` marker.

## Crashes

Start from the user's `~/.local/state/scout/scout.log`. Its startup line names
the version and commit, a crash writes a backtrace, and every window close
records what closed it. Reproduce the crash in a unit test under
AddressSanitizer before fixing it. A use-after-free often survives normal runs,
so a passing normal build proves nothing.

## Working with Tony

- For a visual or interaction change, mock up a few options as HTML first and
  let him pick before building.
- Every build he tests gets a new version in `CMakeLists.txt` and `package.nix`.
- Handing over a build:
  1. Push scout.
  2. In `~/dotfiles`, run `nix flake update scout`, then build
     `nixosConfigurations.sietch.config.system.build.toplevel` and the same
     for `jacurutu`.
  3. Run the harness against the binary the dotfiles produce.
  4. Commit only `flake.lock` and push. If `~/dotfiles` holds unpushed commits
     from another session, commit on top of `origin/nixos` in a worktree and
     push only yours.
  5. Tell him with `harkctl notify`.

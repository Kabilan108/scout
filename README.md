# scout

A keyboard-first, ranger-style file manager for Wayland, built on Qt 6.

Three columns: the parent folder, the current folder, and a live preview of
whatever is under the cursor. Vim keys, a which-key popup for multi-key
bindings, a fuzzy finder, and a command palette. Every action is a named command,
so keys, the `Ctrl-k` palette, the right-click menu and the config file all go
through the same path. The mouse works, including dragging files out to other
apps, but nothing needs it.

Inspired by rockorager's [Strider](https://github.com/rockorager).

## Run it

```sh
nix run github:Kabilan108/scout                          # current directory
nix run github:Kabilan108/scout -- ~/Downloads
nix run github:Kabilan108/scout -- ~/Pictures/photo.jpg  # reveals the file
nix run --refresh github:Kabilan108/scout                # pick up new commits
```

From a checkout: `nix run .` or `nix develop`, then
`cmake -B build -G Ninja && cmake --build build && ./build/scout`.

## Keys

Press `Ctrl-k` for the command palette, which lists every command with its
keys, or a prefix key (`g`, `y`, `d`, `p`, `s`, `z`, `u`) and wait for the
which-key popup. The palette's "Show key map" row (the `keymap` command) opens a cheat
sheet grouped by task, built from your current bindings.

| Keys | Action |
| --- | --- |
| `h` `j` `k` `l` / arrows | parent, down, up, enter (or open a file) |
| `Enter` | open with the default app |
| `gg` `G` `5j` `Ctrl-d` `Ctrl-u` | top, bottom, counts, half pages (`PageDown` `PageUp` for whole pages) |
| `H` `L` | back and forward in history |
| `gh` `gd` | home, your downloads folder (`XDG_DOWNLOAD_DIR` from `~/.config/user-dirs.dirs`) |
| `gc` `gr` `g/` `gb` | ~/.config, ~/repos, /, the trash (bin) |
| `gn` / `Ctrl-t` | new tab here |
| `gt` `gp` / `Ctrl-Tab` `Ctrl-Shift-Tab` / `Alt-1`…`Alt-9` | next, previous, or Nth tab |
| `q` / `Ctrl-w` | close the tab (the window, if it's the last); `Ctrl-q` closes the window |
| `uq` | reopen the last closed tab |
| `Ctrl-k` | command palette: search commands and keys, or type a command line |
| `Ctrl-f` | fuzzy find below the current folder |
| `/` `n` `N` | jump to a matching name or glob (`*.png`) |
| `f` / `zf` | filter the folder as you type, by text or glob (`Esc` clears) |
| `Space` `*` `Ctrl-a` `uv` | mark and move down, invert, mark all, unmark |
| `v` / `V` | visual mode: moving the cursor marks every row from where you pressed `v`; `v`, `V`, or `Esc` stops and keeps the marks |
| `yy` `dd` `pp` | copy, cut, paste (also `Ctrl-c` `Ctrl-x` `Ctrl-v`) |
| `pl` | paste as symlinks |
| `yp` `yn` `yd` | copy path, name, or folder path as text |
| `dD` / `Delete` | move to trash |
| `dX` / `Shift-Delete` | delete permanently (asks `y/N`) |
| `r` / `cw` / `F2` | rename (the name without its extension is preselected); with several marked, bulk rename |
| `R` | bulk rename the marked items, or the whole folder, in `$EDITOR` |
| `uu` / `Ctrl-z` | undo the last trash, move, rename, or copy |
| `ur` | restore the trashed item under the cursor to where it came from |
| `a` / `A` | create a file, or a folder if the name ends in `/` |
| `o` | open with… (recommended apps first, then everything else) |
| `zh` / `Ctrl-h` | toggle hidden files |
| `sn` `ss` `sm` `se` `sr` | sort by name, size, modified, extension; reverse |
| `J` `K` | scroll the preview |
| `T` | terminal in this folder |
| `w` | tasks: progress of copies and moves (`x` cancels) |
| `Alt-Enter` | properties under the preview, following the cursor (again, or `Esc`, closes) |
| `Ctrl-n` | new window here |
| `Ctrl-=` `Ctrl--` `Ctrl-0` | zoom in, out (remembered across restarts), and back to the configured size |

Marks persist across folders and tabs, so you can collect files from several
places and then copy, trash, or drag them together. Visual mode ends when you
leave the folder or run a command such as `yy` or `dD`, which then acts on
everything marked.

## Palette, search, and properties

`Ctrl-k` opens the command palette. Each row is a command: what it does, its
name, and every key bound to it. Bindings that carry arguments, such as
`sn` (`sort name`), get rows of their own. Typing searches descriptions,
names and keys. Type a command with arguments (`cd ~/notes`, `set hidden true`,
`sort size`) and `Enter` runs it as typed; `Tab` completes paths and options.
A command that needs arguments fills in its name and waits for them. The
`command` and `help` commands open the same palette, so either can be mapped
to another key.

`/` opens a search line above the status bar. Matches light up in the folder
as you type, and the line shows which match you're on, such as "3 of 41". `Enter`
keeps the search for `n` and `N`; `Esc` puts the cursor back. Text with `*`,
`?` or `[` is a glob that has to match the whole name, so `*.md` finds
Markdown files and `file?.txt` finds `file2.txt` but not `file10.txt`. An
uppercase letter makes the match case sensitive. The filter (`f`) takes the
same text and globs.

`Alt-Enter` docks a properties shelf under the preview with the size,
modification and creation times, permissions and owner, location, and link
target. Folder totals are counted in the background and fill in as the count
runs. With
several items marked it shows their count and total size. It follows the
cursor until you press `Alt-Enter` again, or `Esc` when there is nothing
else to clear.

## Remembered between runs

`~/.local/state/scout/state.json` (`$XDG_STATE_HOME`) holds what scout
remembers, apart from the config so a Nix-managed `scout.conf` never fights it:

- The zoomed text size. It wins over `set font-size` in the config;
  `Ctrl-0` goes back to the configured size and forgets it.
- The tabs (their folders and which was active) of the window you closed
  last. `scout` with no path reopens them, skipping folders that are gone.
  `scout <path>`, "Show in folder", file chooser dialogs and `Ctrl-n` open
  just their folder.

## Undo, trash, and bulk rename

Undo (`uu`) walks back through trashing, moves, renames, bulk renames, and
copies (an undone copy goes to the trash). It never replaces a file: if
something new now sits where an item would return to, that item stays put and
scout tells you.

In the trash (`gb`), `ur` restores the item under the cursor, or everything
marked, to its original location. `:empty-trash` asks before deleting
everything in it.

`R` writes the names to a temporary file and opens it in your editor
(`set editor`, then `$VISUAL`, then `$EDITOR`, then nvim/vim/vi). Terminal
editors open in your terminal (`$TERMINAL`, or the first of ghostty, kitty,
foot, alacritty, wezterm, konsole, gnome-terminal, xterm). Change the lines,
save, quit, and scout shows the renames and asks before applying them.
Swaps such as `a → b, b → a` work. If any new name is taken by a file outside
the rename, or appears twice, nothing is renamed. For GUI editors that need a
flag to wait, put it in the setting: `set editor code --wait`.

## Drag and drop

- Drag any row out to another app. If that row is marked, all marked files go.
- The status bar shows a `⠿ drag N` chip whenever files are marked. Mark from
  the keyboard, then grab the chip with the mouse.
- The image preview is a drag source too.
- Drop onto a folder row to put files there, or onto empty space for the
  current folder. Drops from other apps copy; drags inside scout move. Hold
  `Shift` to move, `Ctrl` to copy, `Ctrl+Shift` to link.

Copy and paste uses the same clipboard formats as Nautilus and Dolphin, so you
can copy in one and paste in the other.

## Previews

Folders, text with syntax highlighting (KDE's syntax-highlighting library),
images, archives (libarchive), the first page of PDFs (`pdftoppm`), video
thumbnails (`ffmpegthumbnailer`), a hex dump for binaries, and an info card
for everything else.

## Theme

scout follows your Stylix palette (`~/.config/stylix/palette.json`) and
reloads it when it changes. Without Stylix it uses Catppuccin Mocha or Latte,
following the desktop portal's light/dark setting. Override with
`set theme dark|light|auto`.

## Config

`~/.config/scout/scout.conf` is a list of commands, run at startup:

```
set hidden true
set font JetBrains Mono
set font-size 11
set sort modified
set editor nvim
map gw cd ~/work
map gn cd ~/notes
map <C-t> terminal
unmap q
```

`set` takes `hidden`, `dirs-first`, `reverse`, `preview` and `line-numbers`
(line numbers in text previews, on by default) (`true`, `false` or `toggle`), `sort` (`name`, `size`, `modified`, `extension`, optionally
followed by `reverse`), `theme` (`auto`, `dark`, `light`), `font`,
`font-size` (6 to 32) and `editor`.

Problems show up in the status bar at startup and in the log. To check a
config without starting scout:

```console
$ scout --check-config            # or: scout --check-config path/to/scout.conf
scout.conf: /home/you/.config/scout/scout.conf
info:
  line 7: map gd overrides the default gd (cd /home/you/downloads)
  line 9: unmap q removes the default q (close-tab)
errors:
  line 3: unknown command frobnicate
  line 8: map g is a prefix of other bindings (g/, gb, gc, gd, gg, gh and 5 more), so it only runs after a pause
2 errors
```

Errors are unknown commands (inside `map` too), unknown keys, unknown `set`
options or bad values, and bindings that are a prefix of another binding or
extend one (scout then has to wait to see whether more keys follow). The
default bindings never do that, so only conflicts the config introduces are
reported. The exit status is 1 when there are errors.

### Home Manager

The flake has a home-manager module (`homeManagerModules.default`, also
`homeModules.default`) that installs scout and writes `scout.conf`:

```nix
{
  imports = [ inputs.scout.homeManagerModules.default ];
  programs.scout = {
    enable = true;
    settings = {
      hidden = true;
      sort = "modified";
      font = "JetBrains Mono";
      font-size = 11;
      editor = "nvim";
    };
    bookmarks = {          # g<key> goes to the folder
      w = "~/work";
      n = "~/notes";
    };
    keybindings = {
      "<C-t>" = "terminal";
      q = null;            # null removes a default binding
    };
    extraConfig = ''
      map gx open
    '';
  };
}
```

The package defaults to `pkgs.scout` when scout's overlay provides it, and to
the flake's own package otherwise. The generated file is run through
`scout --check-config` while it builds, so a config with errors fails the
build with the report in the log. With nothing configured, the module leaves
an existing `scout.conf` alone.

## Desktop integration

`scout --service` is a D-Bus service that owns two names. D-Bus starts it on
demand, and it exits after ten idle minutes with no windows.

- `org.freedesktop.FileManager1`: "Show in folder" in browsers, download
  managers, and other apps opens a scout window with the file selected.
- `org.freedesktop.impl.portal.desktop.scout`: a FileChooser backend for
  xdg-desktop-portal, so open and save dialogs become scout windows.

In a chooser window everything works as usual, plus:

| Dialog | Keys |
| --- | --- |
| Open | `Enter`/`l` on a file chooses it. With several allowed, mark with `Space` and press `Ctrl-Enter`. |
| Select folder | `l` goes into a folder, `Ctrl-Enter` selects the folder you're in. |
| Save | The name field has focus. `Enter` saves into the folder you're in (asking before replacing). `Tab` moves between the name and the folders. |
| Any | `Esc` (with nothing left to clear) or `q` cancels; `Alt-f` cycles the file type filter. |

On NixOS:

```nix
xdg.portal = {
  enable = true;
  extraPortals = [ scout ];
  # This replaces niri's own niri-portals.conf, so restate its default.
  config.niri = {
    default = [ "gnome" "gtk" ];
    "org.freedesktop.impl.portal.FileChooser" = [ "scout" "gnome" "gtk" ];
  };
};
# Nautilus ships a FileManager1 service file too; priority lets scout's win.
home.packages = [ (lib.hiPrio scout) ];
xdg.mimeApps.defaultApplications."inode/directory" = "scout.desktop";
```

Apps decide whether to use the portal. Chromium, Electron apps, and
Flatpaks do by default. GTK 3 apps need `GTK_USE_PORTAL=1`, GTK 4 apps
`GDK_DEBUG=portals`, Firefox `widget.use-xdg-desktop-portal.file-picker = 1`,
and Qt apps `QT_QPA_PLATFORMTHEME=xdgdesktopportal`.

New D-Bus service files take effect after logging out and back in (or
`busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig`),
and the portal routing after `systemctl --user restart xdg-desktop-portal`.

Without NixOS, make scout the default folder handler with
`xdg-mime default scout.desktop inode/directory`.

## Logs and crashes

Every scout process (a window from the launcher or a terminal, the D-Bus
service, file chooser windows) appends to
`~/.local/state/scout/scout.log` (`$XDG_STATE_HOME`), and to stderr. At 1 MB
it moves to `scout.log.1` and a new file starts. Each line has a timestamp
and the process id:

```
2026-10-28 19:20:01.123 [4242] I scout 0.3.0 (8a6e817) starting: mode window, platform wayland, args []
2026-10-28 19:24:13.502 [4242] E no such directory: ~/nope
2026-10-28 19:31:40.017 [4242] I window closed by close-tab on the last tab (key q): /home/you/repos (1 tab)
2026-10-28 19:31:40.020 [4242] I exit status 0
```

It records Qt's own warnings, every error and warning shown in the status
bar, failed copies, moves and deletes with all their errors, which key or
command closed each tab and window (or that the window manager did), and the
exit status. A window that vanished without a `window closed` line and an
`exit status` line didn't close normally: look for `exit() called before
main() returned`, a Qt message just before it (`The Wayland connection
broke` means the compositor connection died, and Qt exits without a core
dump), or a crash.

On a crash (`SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGFPE`, `SIGILL`) scout writes
the signal and a backtrace to the log, then lets the signal kill it as
usual, so `coredumpctl` still gets the core. The Nix package keeps symbols
and line tables, so `coredumpctl gdb scout` and the logged backtrace both
show function names. `SIGTERM`, `SIGHUP` and `SIGINT` are logged too.

`scout --version` prints the version and the git commit it was built from.

## Not done yet

- Chooser windows open as ordinary windows rather than attached to the app
  that asked (xdg-foreign); niri places them like any new window.
- Mounting and ejecting drives.

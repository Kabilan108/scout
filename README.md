# scout

A keyboard-first, ranger-style file manager for Wayland, built on Qt 6.

Three columns: the parent folder, the current folder, and a live preview of
whatever is under the cursor. Vim keys, a which-key popup for multi-key
bindings, a fuzzy finder, and a command line. Every action is a named command,
so keys, the `:` palette, the right-click menu and the config file all go
through the same path. The mouse works, including dragging files out to other
apps, but nothing needs it.

Inspired by rockorager's [Strider](https://github.com/rockorager).

## Run it

```sh
nix run git+ssh://git@github.com/Kabilan108/scout                          # current directory
nix run git+ssh://git@github.com/Kabilan108/scout -- ~/Downloads
nix run git+ssh://git@github.com/Kabilan108/scout -- ~/Pictures/photo.jpg  # reveals the file
nix run git+ssh://git@github.com/Kabilan108/scout --refresh                # pick up new commits
```

The repository is private, so the `git+ssh` form is the one that works
everywhere your SSH key does. If it is made public, `nix run github:Kabilan108/scout`
works too.

From a checkout: `nix run .` or `nix develop`, then
`cmake -B build -G Ninja && cmake --build build && ./build/scout`.

## Keys

Press `?` for the full, searchable list, or a prefix key (`g`, `y`, `d`, `s`,
`z`) and wait for the which-key popup.

| Keys | Action |
| --- | --- |
| `h` `j` `k` `l` / arrows | parent, down, up, enter (or open a file) |
| `Enter` | open with the default app |
| `gg` `G` `5j` `Ctrl-d` `Ctrl-u` | top, bottom, counts, half pages |
| `H` `L` | back and forward in history |
| `gh` `gd` `gD` `gp` `gv` `gM` | home, Downloads, Documents, Pictures, Videos, Music |
| `gc` `gr` `g/` `gb` `gm` | ~/.config, ~/repos, /, the trash (bin), mounted drives |
| `gn` / `Ctrl-t` | new tab here |
| `gt` `gT` / `Ctrl-Tab` `Ctrl-Shift-Tab` / `Alt-1`…`Alt-9` | next, previous, or Nth tab |
| `q` / `Ctrl-w` | close the tab (the window, if it's the last); `Ctrl-q` closes the window |
| `uq` | reopen the last closed tab |
| `gf` / `Ctrl-p` | fuzzy find below the current folder |
| `/` `n` `N` | jump to a matching name |
| `f` / `zf` | filter the folder as you type (`Esc` clears) |
| `Space` `v` `Ctrl-a` `uv` | mark and move down, invert, mark all, unmark |
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
| `:` | command line with completion |
| `!` | run a shell command here; marked files are `"$@"` |
| `T` | terminal in this folder |
| `w` | tasks: progress of copies and moves (`x` cancels) |
| `Alt-Enter` | properties, including total folder size |
| `Ctrl-n` | new window here |
| `Ctrl-=` `Ctrl--` | zoom |

Marks persist across folders and tabs, so you can collect files from several
places and then copy, trash, or drag them together.

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

## Not done yet

- Chooser windows open as ordinary windows rather than attached to the app
  that asked (xdg-foreign); niri places them like any new window.
- Mounting and ejecting drives; `gm` only browses what is already mounted.

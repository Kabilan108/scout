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
nix run github:Kabilan108/scout            # opens the current directory
nix run github:Kabilan108/scout -- ~/Downloads
nix run github:Kabilan108/scout -- ~/Pictures/photo.jpg   # reveals the file
```

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
| `gh` `gd` `gD` `gp` `gc` `gr` `g/` `gt` | home, Downloads, Documents, Pictures, ~/.config, ~/repos, /, trash |
| `gf` / `Ctrl-p` | fuzzy find below the current folder |
| `/` `n` `N` | jump to a matching name |
| `f` / `zf` | filter the folder as you type (`Esc` clears) |
| `Space` `v` `Ctrl-a` `uv` | mark and move down, invert, mark all, unmark |
| `yy` `dd` `pp` | copy, cut, paste (also `Ctrl-c` `Ctrl-x` `Ctrl-v`) |
| `pl` | paste as symlinks |
| `yp` `yn` `yd` | copy path, name, or folder path as text |
| `dD` / `Delete` | move to trash |
| `dX` / `Shift-Delete` | delete permanently (asks `y/N`) |
| `r` / `cw` / `F2` | rename (the name without its extension is preselected) |
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
| `q` | quit |

Marks persist across folders, so you can collect files from several places
and then copy, trash, or drag them together.

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
map gw cd ~/work
map gn cd ~/notes
map <C-t> terminal
unmap q
```

## Making it the default folder handler

With scout installed into your profile (for example by adding the flake's
package to `home.packages`), run:

```sh
xdg-mime default scout.desktop inode/directory
```

## Not done yet

- Acting as the desktop's file chooser (an xdg-desktop-portal backend)
- `org.freedesktop.FileManager1`, so "Show in folder" from browsers opens scout
- Tabs, bulk rename in `$EDITOR`, undo for trash

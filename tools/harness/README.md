# Headless verification

These scripts run the real scout binary in a headless sway that only they can
see. It gets XDG dirs of its own, so the user's desktop, config, state and trash
stay untouched. Unit tests run offscreen and miss real Wayland input, drawing
and D-Bus activation. Use these scripts for those.

Run them inside `nix develop`, which provides sway, grim, wtype, dbus and the
portal. Output lands in `~/.cache/scout-harness` (`$SCOUT_HARNESS_DIR`):
screenshots in `out/`, scout's log in `xdg/state/scout/scout.log`.

## Which script

| Change | Run |
| --- | --- |
| Anything the user sees: keys, prompts, popups, previews, colors, layout | `ui-tour.sh` |
| File chooser dialogs, "Show in folder", `src/service.cpp`, startup | `portal-e2e.sh` |
| A build about to go to the user | both, against the dotfiles binary |

## Steps

1. Build the binary: `nix build .#scout`. The user runs a different build: their
   dotfiles compile scout against their own nixpkgs, so its Qt can differ. Before
   a hand-off, test that one too:

   ```sh
   p=$(nix build --no-link --print-out-paths \
       ~/dotfiles#nixosConfigurations.jacurutu.config.home-manager.users.kabilan.home.path)
   bin=$(readlink -f "$p/bin/scout")
   ```

2. Run the script and capture its exit status:

   ```sh
   nix develop -c tools/harness/ui-tour.sh result/bin/scout > /tmp/ui-tour.log 2>&1; echo "exit=$?"
   ```

   Exit 0 means scout started, never crashed, and quit cleanly every time. It
   says nothing about how things look.
3. Read every screenshot of the surfaces you changed, in both `dark-*` and
   `light-*`. Done when each changed surface appears in a shot and reads
   correctly. If a shot doesn't show the surface (a popup that never opened),
   fix the flow and run again.
4. For dialog changes, `portal-e2e.sh` checks its own results: exit 0 means
   every dialog returned the expected path, the cancelled one closed, and "Show
   in folder" opened a window.

## Adding a flow

Add steps to `ui-tour.sh`'s `tour` function using the helpers in `lib.sh`:
`launch`, `keys`, `keys_during`, `shot`, `quit_scout`, `fail`. Each `wtype` call
is one virtual keyboard session, and that drives most surprises:

- A run of keys goes in one word: `keys vjj`. Separate words get a space typed
  between them, and Space marks a file.
- The keyboard leaves the window when the session ends, which cancels pending
  prefix keys and hides which-key. Screenshot a popup that waits on more keys
  with `keys_during`, and leave Escape out of its keys.
- Background jobs: wait on the job's pid (`wait $!`). A bare `wait` also waits
  for scout and hangs.

## Safety

`start_sway` refuses to run unless a new headless display came up, so a broken
start can't put windows on the user's session. Keep new flows pointed at
`$HARNESS/tree` or other folders under `$HARNESS`, and never at the user's own
files.

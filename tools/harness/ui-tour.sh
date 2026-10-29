#!/usr/bin/env bash
# ui-tour.sh SCOUT_BIN: screenshot every surface, in the dark then the light
# theme, into $OUT. Exits 1 if scout crashes, fails to start, or exits uncleanly.
source "$(dirname "$0")/lib.sh"
BIN=${1:?usage: ui-tour.sh path/to/scout}
start_sway

# A folder with something for every preview kind.
TREE=$HARNESS/tree
rm -rf "$TREE"
mkdir -p "$TREE/folder/inner" "$TREE/notes"
for i in $(seq 40); do echo "/* block $i"; echo "still a comment */ int x$i = $i;"; done > "$TREE/code.cpp"
printf '# Notes\n\nSome *markdown* with a list:\n\n- one\n- two\n' > "$TREE/notes/today.md"
printf 'plain text\n' > "$TREE/readme.txt"
head -c 512 /dev/urandom > "$TREE/blob.bin"
grim "$TREE/picture.png"

tour() {
    local theme=$1
    printf 'set theme %s\n' "$theme" > "$XDG_CONFIG_HOME/scout/scout.conf"
    launch "$BIN" "$TREE/code.cpp"
    shot "$theme-01-text-preview"
    keys gg
    shot "$theme-02-folder"
    keys -M ctrl k -m ctrl
    shot "$theme-03-palette"
    keys hidden
    shot "$theme-04-palette-query"
    keys -k BackSpace -k BackSpace -k BackSpace -k BackSpace -k BackSpace -k BackSpace zzzq
    shot "$theme-05-palette-empty"
    keys -k Escape -M ctrl f -m ctrl
    sleep 0.5
    keys tod
    shot "$theme-06-find"
    keys -k Escape /o
    shot "$theme-07-search"
    keys -k Escape -M alt -k Return -m alt
    shot "$theme-08-properties-shelf"
    keys -M alt -k Return -m alt
    keys_during 1.2 "$theme-09-whichkey" g
    keys vjj
    shot "$theme-10-visual"
    keys -k Escape -k Escape w
    shot "$theme-11-tasks"
    keys -k Escape -M ctrl k -m ctrl "show key" -k Return
    shot "$theme-12-keymap"
    keys -k Escape
    quit_scout
}

tour dark
tour light
ls "$OUT"/*.png
[ "$FAILED" = 0 ] && echo "ui-tour: OK" || echo "ui-tour: FAILED"
exit "$FAILED"

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
cat > "$TREE/paper.pdf" << 'PDF'
%PDF-1.4
1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj
2 0 obj << /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >> endobj
3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Contents 4 0 R
  /Resources << /Font << /F1 5 0 R >> >> >> endobj
4 0 obj << >> stream
BT /F1 28 Tf 40 90 Td (PDF page one) Tj ET
endstream endobj
5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj
6 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Contents 7 0 R
  /Resources << /Font << /F1 5 0 R >> >> >> endobj
7 0 obj << >> stream
BT /F1 28 Tf 40 90 Td (PDF page two) Tj ET
endstream endobj
trailer << /Root 1 0 R >>
%%EOF
PDF
# Black strokes on a transparent background, 24x24: shows the checkerboard and
# that small SVGs are drawn at the pane's size.
printf '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24"><path d="M4 20 L12 4 L20 20 Z" stroke="#000" stroke-width="1.5" fill="none"/></svg>\n' > "$TREE/icon.svg"
# Two 8x8 frames, red then blue, looping.
printf '\x47\x49\x46\x38\x39\x61\x08\x00\x08\x00\xf0\x00\x00\xff\x00\x00\x00\x00\x00\x21\xff\x0b\x4e\x45\x54\x53\x43\x41\x50\x45\x32\x2e\x30\x03\x01\x00\x00\x00\x21\xf9\x04\x00\x05\x00\x00\x00\x2c\x00\x00\x00\x00\x08\x00\x08\x00\x00\x02\x07\x84\x8f\xa9\xcb\xed\x5d\x00\x00\x21\xf9\x04\x00\x05\x00\x00\x00\x2c\x00\x00\x00\x00\x08\x00\x08\x00\x80\x00\x00\xff\x00\x00\x00\x02\x07\x84\x8f\xa9\xcb\xed\x5d\x00\x00\x3b' > "$TREE/blink.gif"

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
    launch "$BIN" "$TREE/paper.pdf"
    shot "$theme-13-pdf-preview"
    keys J
    shot "$theme-14-pdf-page-two" 1.5
    quit_scout
    launch "$BIN" "$TREE/icon.svg"
    shot "$theme-15-svg-checkerboard"
    quit_scout
    launch "$BIN" "$TREE/blink.gif"
    shot "$theme-16-gif-frame-a" 0.1
    shot "$theme-17-gif-frame-b" 0.05
    quit_scout
}

tour dark
tour light
ls "$OUT"/*.png
[ "$FAILED" = 0 ] && echo "ui-tour: OK" || echo "ui-tour: FAILED"
exit "$FAILED"

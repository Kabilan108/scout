#!/usr/bin/env bash
# portal-e2e.sh SCOUT_BIN: drive real file chooser dialogs and "Show in folder"
# through D-Bus on a private session bus. Exits 1 unless every dialog returns
# the expected path.
source "$(dirname "$0")/lib.sh"
BIN=$(realpath "${1:?usage: portal-e2e.sh path/to/scout}")
: "${SCOUT_XDP:?run inside nix develop, which sets SCOUT_XDP}"
start_sway

E=$HARNESS/portal
rm -rf "$E"
mkdir -p "$E/share/dbus-1/services" "$E/portals" "$E/files/sub"
echo "# a" > "$E/files/a.md"
echo b > "$E/files/b.txt"
echo c > "$E/files/c.md"

# The private bus starts scout and a wrapped xdg-desktop-portal on demand. The
# wrapper points the portal at scout.portal only, so the system's portal
# backends never answer.
for name in org.freedesktop.FileManager1 org.freedesktop.impl.portal.desktop.scout; do
    printf '[D-BUS Service]\nName=%s\nExec=%s --service\n' "$name" "$BIN" > "$E/share/dbus-1/services/$name.service"
done
cp "$(dirname "$BIN")/../share/xdg-desktop-portal/portals/scout.portal" "$E/portals/"
printf '[preferred]\ndefault=scout\n' > "$E/portals/scouttest-portals.conf"
cat > "$E/xdp.sh" <<EOF
#!/usr/bin/env bash
export XDG_DESKTOP_PORTAL_DIR=$E/portals XDG_CURRENT_DESKTOP=scouttest
unset NIX_XDG_DESKTOP_PORTAL_DIR
exec $SCOUT_XDP --verbose >> $OUT/xdp.log 2>&1
EOF
chmod +x "$E/xdp.sh"
printf '[D-BUS Service]\nName=org.freedesktop.portal.Desktop\nExec=%s/xdp.sh\n' "$E" \
    > "$E/share/dbus-1/services/org.freedesktop.portal.Desktop.service"
rm -f "$OUT/xdp.log"

cat > "$E/dialogs.sh" <<'EOF'
source "$LIB"
# Portal paths are byte strings ("ay"); a bare b'...' in gdbus loses all but two bytes.
bytes() { printf '%s\0' "$1" | od -An -tx1 -v | tr -s ' \n' ' ' | sed 's/^ //; s/ $//; s/ /, 0x/g; s/^/[byte 0x/; s/$/]/'; }
FOLDER=$(bytes "$E/files")
open_dialog() { # METHOD TITLE OPTIONS
    gdbus call --session -d org.freedesktop.portal.Desktop -o /org/freedesktop/portal/desktop \
        -m "org.freedesktop.portal.FileChooser.$1" "" "$2" "$3" > /dev/null
    for _ in $(seq 60); do swaymsg -t get_tree | grep -q "\"name\": \"$2" && return 0; sleep 0.2; done
    fail "no window titled $2"
}
open_dialog OpenFile "Pick a note" "{'handle_token': <'t1'>, 'current_folder': <$FOLDER>, 'filters': <[('Markdown', [(uint32 0, '*.md')]), ('All', [(uint32 0, '*')])]>}"
shot portal-1-open 1
keys jj -k Return
sleep 1.5
open_dialog SaveFile "Save report" "{'handle_token': <'t2'>, 'current_name': <'report.txt'>, 'current_folder': <$FOLDER>}"
shot portal-2-save 1
keys -M ctrl a -m ctrl final.txt -k Return
sleep 1.5
open_dialog OpenFile "Choose folder" "{'handle_token': <'t3'>, 'directory': <true>, 'current_folder': <$FOLDER>}"
sleep 1
keys l -s 400 -M ctrl -k Return -m ctrl
sleep 1.5
open_dialog OpenFile "Find one" "{'handle_token': <'t4'>, 'current_folder': <$FOLDER>}"
sleep 1
keys -M ctrl f -m ctrl -s 600 b.t -s 600 -k Return -s 500 -k Return
sleep 1.5
open_dialog OpenFile "Cancel me" "{'handle_token': <'t5'>}"
sleep 0.8
keys -k Escape
sleep 1.5
swaymsg -t get_tree | grep -q '"name": "Cancel me' && fail "the cancelled dialog is still open"
gdbus call --session -d org.freedesktop.FileManager1 -o /org/freedesktop/FileManager1 \
    -m org.freedesktop.FileManager1.ShowItems "['file://$E/files/c.md']" "" > /dev/null
sleep 2
swaymsg -t get_tree | grep -q '"name": "scout — ' || fail "Show in folder opened no window"
shot portal-3-show-in-folder 0
echo "FAILED=$FAILED" > "$E/result"
EOF
LIB=$(dirname "$0")/lib.sh
LIB=$(realpath "$LIB") E=$E OUT=$OUT XDG_DATA_DIRS=$E/share:${XDG_DATA_DIRS:-/run/current-system/sw/share} \
    dbus-run-session -- bash -c 'source "$LIB"; FAILED=0; SWAY_PID=; source "$E/dialogs.sh"'
pkill -f "$BIN --service"
grep -q "FAILED=0" "$E/result" 2> /dev/null || FAILED=1

# The portal logs every path it hands back to the calling app.
for want in files/c.md files/final.txt files/sub files/b.txt; do
    grep -q "convert uri file://$E/$want " "$OUT/xdp.log" || fail "no dialog returned $want"
done
[ "$FAILED" = 0 ] && echo "portal-e2e: OK" || echo "portal-e2e: FAILED"
exit "$FAILED"

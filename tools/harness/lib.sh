# Sourced by the harness scripts. Runs scout inside a headless sway that only
# the harness sees, with XDG dirs of its own, so the user's session, config,
# state and trash stay untouched.
set -u

HARNESS=${SCOUT_HARNESS_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/scout-harness}
OUT=$HARNESS/out
FAILED=0
REAL_WAYLAND=${WAYLAND_DISPLAY:-}

fail() {
    echo "FAIL: $*" >&2
    FAILED=1
}

start_sway() {
    # Pinned, since XDG_CACHE_HOME changes below and child scripts source this file.
    export SCOUT_HARNESS_DIR=$HARNESS
    mkdir -p "$HARNESS" "$OUT"
    rm -f "$HARNESS/wl" "$HARNESS/sock"
    cat > "$HARNESS/sway.conf" <<EOF
output HEADLESS-1 resolution 1280x800 scale 1
default_border none
gaps inner 0
exec sh -c 'echo \$WAYLAND_DISPLAY > $HARNESS/wl; echo \$SWAYSOCK > $HARNESS/sock'
EOF
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman WAYLAND_DISPLAY= DISPLAY= \
        sway -c "$HARNESS/sway.conf" > "$HARNESS/sway.log" 2>&1 &
    SWAY_PID=$!
    for _ in $(seq 50); do
        [ -s "$HARNESS/sock" ] && break
        sleep 0.1
    done
    WAYLAND_DISPLAY=$(cat "$HARNESS/wl" 2>/dev/null || true)
    SWAYSOCK=$(cat "$HARNESS/sock" 2>/dev/null || true)
    export WAYLAND_DISPLAY SWAYSOCK
    unset DISPLAY
    if [ -z "$WAYLAND_DISPLAY" ] || [ "$WAYLAND_DISPLAY" = "$REAL_WAYLAND" ] ||
        ! swaymsg -t get_version > /dev/null 2>&1; then
        echo "harness: headless sway did not start; refusing to run against the real session" >&2
        kill "$SWAY_PID" 2> /dev/null
        exit 1
    fi
    export XDG_CONFIG_HOME=$HARNESS/xdg/config XDG_STATE_HOME=$HARNESS/xdg/state
    export XDG_DATA_HOME=$HARNESS/xdg/data XDG_CACHE_HOME=$HARNESS/xdg/cache
    rm -rf "$HARNESS/xdg"
    mkdir -p "$XDG_CONFIG_HOME/scout" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
    trap stop_sway EXIT
}

stop_sway() {
    [ -n "${SCOUT_PID:-}" ] && kill "$SCOUT_PID" 2> /dev/null
    kill "$SWAY_PID" 2> /dev/null
}

# launch BIN [ARGS...]: start scout in the background; its pid lands in SCOUT_PID.
launch() {
    LOG_LINES=$(cat "$XDG_STATE_HOME/scout/scout.log" 2> /dev/null | wc -l)
    "$@" >> "$OUT/scout.stderr" 2>&1 &
    SCOUT_PID=$!
    sleep 2.5
    kill -0 "$SCOUT_PID" 2> /dev/null || fail "scout exited at startup (see $XDG_STATE_HOME/scout/scout.log)"
}

# keys ARGS...: one wtype session. A run of keys goes in one word ("vjj");
# separate words get a space typed between them, and Space is a scout key.
# The virtual keyboard leaves the window when the session ends, which cancels
# pending prefix keys, so screenshot a which-key popup with keys_during.
keys() {
    wtype -s 400 "$@" -s 200
}

# keys_during SECONDS NAME ARGS...: send keys, screenshot after SECONDS while
# that keyboard session is still open, then wait for it. Ending the session
# cancels the pending keys, so leave Escape out of ARGS.
keys_during() {
    local delay=$1 name=$2
    shift 2
    wtype -s 400 "$@" -s 2000 &
    local typing=$!
    sleep "$delay"
    shot "$name" 0
    wait "$typing"
}

# shot NAME [DELAY]: screenshot the headless output to $OUT/NAME.png.
shot() {
    sleep "${2:-0.6}"
    grim "$OUT/$1.png"
}

# quit_scout: Ctrl-q, then check the log for a clean exit and no crash.
quit_scout() {
    keys -M ctrl q -m ctrl
    sleep 1
    if kill -0 "$SCOUT_PID" 2> /dev/null; then
        fail "scout still running after Ctrl-q"
        kill "$SCOUT_PID"
    fi
    local run
    run=$(tail -n +$((LOG_LINES + 1)) "$XDG_STATE_HOME/scout/scout.log")
    grep -q "crash:" <<< "$run" && fail "scout.log has a crash trace"
    grep -q "exit status 0" <<< "$run" || fail "scout.log has no clean exit"
    SCOUT_PID=
}

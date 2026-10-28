#!/bin/sh
# Starts Animal Crossing: City Folk Recompiled and keeps a log of the run (README.txt).
# All arguments are passed on, e.g.  ./CityFolkRecompiled.sh --setup
HERE=$(dirname "$(readlink -f "$0")")
# Portable mode (docs/SETUP.md "Portable mode"): portable.txt or a folder "portable" next to the
# game, or CF_PORTABLE=1, keeps everything in ./portable, this log included. The game decides the
# same way; a program folder that cannot be written keeps the system folders.
PORTABLE=0
case "${CF_PORTABLE:-}" in
    0|false|no|off) ;;
    "") { [ -e "$HERE/portable.txt" ] || [ -d "$HERE/portable" ]; } && PORTABLE=1 ;;
    *) PORTABLE=1 ;;
esac
if [ "$PORTABLE" = 1 ] && mkdir -p "$HERE/portable" 2>/dev/null && [ -w "$HERE/portable" ]; then
    DATA="$HERE/portable"
else
    DATA="${XDG_DATA_HOME:-$HOME/.local/share}/Animal Crossing City Folk Recompiled"
    mkdir -p "$DATA"
fi
LOG="$DATA/last_run.log"
[ -f "$LOG" ] && mv -f "$LOG" "$DATA/previous_run.log"
# Libraries shipped next to the game (none by default; everything is linked in) come first.
if [ -d "$HERE/lib" ]; then
    LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi
# Unpacking a .zip can drop the executable bits.
chmod +x "$HERE/CityFolkRecomp" "$HERE/llvm/bin/clang" "$HERE/llvm/bin/ld.lld" 2>/dev/null
{
    echo "Animal Crossing: City Folk Recompiled - $(date)"
    uname -srm
    grep -E '^PRETTY_NAME=' /etc/os-release 2>/dev/null
    ldd --version 2>/dev/null | head -n 1
    echo "XDG_SESSION_TYPE=${XDG_SESSION_TYPE} WAYLAND_DISPLAY=${WAYLAND_DISPLAY} DISPLAY=${DISPLAY}"
    echo "data: $DATA"
    echo "args: $*"
    echo "----"
} > "$LOG"
# The exit status goes through a temporary file (outside the data folder, which the game may move
# to or from portable mode while this script waits).
STATUS_FILE=$(mktemp 2>/dev/null || echo "/tmp/cf-exit-status.$$")
rm -f "$STATUS_FILE"
{ "$HERE/CityFolkRecomp" "$@"; echo $? > "$STATUS_FILE"; } 2>&1 | tee -a "$LOG"
STATUS=$(cat "$STATUS_FILE" 2>/dev/null || echo 1)
rm -f "$STATUS_FILE"
if [ "$STATUS" != "0" ]; then
    echo "Animal Crossing: City Folk Recompiled exited with status $STATUS. Log: $LOG" >&2
fi
exit "$STATUS"

#!/bin/sh
# Checks that a Wi-Fi remote SET applies live: sets the frontlight brightness,
# reads the light back with SET get, then restores it.
# Usage: scripts/remote_set_check.sh <device-ip> <pin-file>
set -eu
ip=$1
pin=$(cat "$2")
cmd() { curl -s --data-urlencode "token=$pin" --data-urlencode "cmd=$1" "http://$ip/api/cmd"; }
old=$(cmd "SET get frontlightBrightness" | cut -d' ' -f3)
new=$(( old == 30 ? 35 : 30 ))
cmd "SET frontlightBrightness $new" >/dev/null
got=$(cmd "SET get frontlightBrightness" | cut -d' ' -f3)
cmd "SET frontlightBrightness $old" >/dev/null
[ "$got" = "$new" ] && echo "PASS: light at $got" || { echo "FAIL: set $new, light at $got"; exit 1; }

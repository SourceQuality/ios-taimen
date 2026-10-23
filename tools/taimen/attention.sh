#!/usr/bin/env bash
# Get the human: alarm sound (3x), spoken message, desktop notification.
# Usage: tools/taimen/attention.sh "phone hung: hold power 10 s"
msg=${1:-"ios-taimen needs you"}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} DISPLAY=${DISPLAY:-:0} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}
notify-send -u normal -t 15000 "ios-taimen" "$msg" 2>/dev/null &
snd=/usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga
for i in 1 2; do timeout 4 paplay --volume=60000 "$snd" 2>/dev/null || timeout 4 pw-play --volume=0.9 "$snd" 2>/dev/null; sleep 0.2; done
spd-say -r -20 -w "Attention. $msg" 2>/dev/null || espeak-ng "Attention. $msg" 2>/dev/null
wait

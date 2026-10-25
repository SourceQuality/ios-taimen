#!/usr/bin/env bash
# Open every app on the first home screen page in turn and screenshot it (tools/taimen/phonectl.py).
#   appsurvey.sh OUTDIR [WAIT_SECONDS]
set -u
out=$1
wait=${2:-10}
mkdir -p "$out"
cd ~/Projects/ios-taimen/port
pc() { timeout 120 python3 tools/taimen/phonectl.py "$@"; }
# name canvas-x canvas-y (iPhone X home screen grid, 1125 x 2436)
apps="FaceTime:170:901 Calendar:431:901 Photos:692:901 Mail:953:901
Notes:170:1187 Reminders:431:1187 Clock:692:1187 News:953:1187
TV:170:1473 Podcasts:431:1473 AppStore:692:1473 Maps:953:1473
Health:170:1759 Wallet:431:1759 Settings:692:1759
Phone:170:2265 Safari:431:2265 Messages:692:2265 Music:953:2265"
pc run unlock 'sleep 3' > /dev/null
for a in $apps; do
	IFS=: read -r name x y <<< "$a"
	pc run home 'sleep 3' "tapc $x $y" "sleep $wait" "shot $out/$name.png" > /dev/null 2>&1 &&
		echo "$(date +%T) $name" || echo "$(date +%T) $name: remote control failed"
done
pc run home > /dev/null

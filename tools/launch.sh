#!/bin/bash
# Starts the game executable directly (Steam must be running).
source "$(dirname "$0")/env.sh"
rm -f "$G/voidvr.log"
powershell -NoProfile -Command "\$env:SteamAppId='2615540'; \$env:SteamGameId='2615540'; Start-Process -FilePath '$G/$EXE' -WorkingDirectory '$G'"
for i in $(seq 1 30); do
  sleep 2
  tasklist | grep -q -i "VOIDBREAKER-Win64" && { echo "game started"; exit 0; }
done
echo "game did not start"; exit 1

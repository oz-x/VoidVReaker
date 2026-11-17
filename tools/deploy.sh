#!/bin/bash
# Stops the game and installs the freshly built mod into Binaries\Win64.
source "$(dirname "$0")/env.sh"
powershell -NoProfile -Command "Stop-Process -Name VOIDBREAKER-Win64-Shipping -Force -ErrorAction SilentlyContinue"
for i in $(seq 1 30); do
  tasklist | grep -q -i "VOIDBREAKER-Win64" || break
  sleep 1
done
sleep 1
cp "$R/build/Release/version.dll" "$R/third_party/build-openxr/src/loader/Release/openxr_loader.dll" "$G/" && echo deployed

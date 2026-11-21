#!/usr/bin/env bash
set -euo pipefail
root=${XDG_DATA_HOME:-$HOME/.local/share}/framestride
app=$root/framestride
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
if [[ ! -x $app ]]; then echo 'Install FrameStride first.' >&2; exit 1; fi
"$app" --version
systemctl --user is-active framestride.service
"$app" --doctor
if [[ ${1:-} == --probe ]]; then
    echo 'Close the dashboard and leave a scene running, or temporarily turn off game-only mode.'
    echo 'Use the SteamVR treadmill backend and enable locomotion. Do this with no game where motion would be unsafe.'
    "$app" --probe
fi

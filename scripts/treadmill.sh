#!/usr/bin/env bash
set -euo pipefail

root=${XDG_DATA_HOME:-$HOME/.local/share}/framestride
driver=$root/driver/framestride
marker=$root/.treadmill-registered

registrar() {
    local candidate
    for candidate in "${FRAMESTRIDE_VRPATHREG:-}" /opt/steamvr/bin/linuxarm64/vrpathreg \
        /opt/steamvr/bin/vrpathreg.sh \
        "$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/vrpathreg.sh"; do
        if [[ -x $candidate ]]; then printf '%s\n' "$candidate"; return 0; fi
    done
    command -v vrpathreg
}

registered() {
    local listing
    listing=$("$1" show) || { echo 'Could not read SteamVR driver registrations.' >&2; return 1; }
    if printf '%s\n' "$listing" | sed -E 's/^[[:space:]]*(\[[^]]*\][[:space:]]*)?//' | grep -Fx -- "$driver" >/dev/null; then
        return 0
    fi
    return 3
}

register_quietly() (
    scratch=$(mktemp -d) || return 1
    trap 'rm -rf -- "$scratch"' EXIT
    printf '#!/bin/sh\nexit 0\n' > "$scratch/xdg-open" || return 1
    chmod +x "$scratch/xdg-open" || return 1
    PATH="$scratch:$PATH" "$1" adddriver "$driver"
)

registration() {
    local reg state=0
    reg=$(registrar) || { echo 'Could not find SteamVR driver registration tool.' >&2; return 1; }
    export LD_LIBRARY_PATH="/opt/steamvr/bin/linuxarm64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    case $1 in
        status) registered "$reg"; return $?;;
        adddriver) if register_quietly "$reg"; then return 0; fi;;
        removedriver) if "$reg" removedriver "$driver"; then return 0; fi;;
        *) return 1;;
    esac
    registered "$reg" || state=$?
    if [[ ( $1 == adddriver && $state == 0 ) || ( $1 == removedriver && $state == 3 ) ]]; then return 0; fi
    echo 'Could not change the FrameStride driver registration.' >&2
    return 1
}

main() {
    [[ $(id -u) != 0 && -f $root/.framestride-install ]] || {
        echo 'Run this as the steamos user after installing FrameStride.' >&2; return 1;
    }
    local config=${XDG_CONFIG_HOME:-$HOME/.config}
    exec 9>"$config/framestride/.install.lock"
    flock -x -w 3 9 || { echo 'FrameStride installation is busy. Try again.' >&2; return 1; }
    case ${1:-} in
        status) registration status;;
        enable)
            # Keep a cleanup marker even if the registrar changes the registry and then fails.
            touch "$marker"
            registration adddriver
            echo 'Treadmill driver registered. Restart SteamVR, then launch FrameStride from the + menu.'
            ;;
        disable)
            registration removedriver
            rm -f -- "$marker"
            echo 'Treadmill driver unregistered. Restart SteamVR to unload it.'
            ;;
        *) echo 'Usage: treadmill.sh status|enable|disable' >&2; return 2;;
    esac
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then main "$@"; fi

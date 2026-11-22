#!/usr/bin/env bash
set -euo pipefail

root=${XDG_DATA_HOME:-$HOME/.local/share}/framestride
driver=$root/driver/framestride
marker=$root/.treadmill-registered

registrar() {
    local candidate
    for candidate in "${FRAMESTRIDE_VRPATHREG:-}" /opt/steamvr/bin/vrpathreg.sh \
        /opt/steamvr/bin/linuxarm64/vrpathreg \
        "$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/vrpathreg.sh"; do
        if [[ -x $candidate ]]; then printf '%s\n' "$candidate"; return 0; fi
    done
    command -v vrpathreg
}

registration() {
    local reg listing
    reg=$(registrar) || { echo 'Could not find SteamVR driver registration tool.' >&2; return 1; }
    export LD_LIBRARY_PATH="/opt/steamvr/bin/linuxarm64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    if "$reg" "$1" "$driver"; then return 0; fi
    listing=$("$reg" show) || { echo 'Could not read SteamVR driver registrations.' >&2; return 1; }
    if printf '%s\n' "$listing" | sed -E 's/^[[:space:]]*(\[[^]]*\][[:space:]]*)?//' | grep -Fx "$driver" >/dev/null; then
        [[ $1 == adddriver ]] && return 0
    else
        [[ $1 == removedriver ]] && return 0
    fi
    echo 'Could not change the FrameStride driver registration.' >&2
    return 1
}

main() {
    [[ $(id -u) != 0 && -f $root/.framestride-install ]] || {
        echo 'Run this as the steamos user after installing FrameStride.' >&2; return 1;
    }
    local config=${XDG_CONFIG_HOME:-$HOME/.config}
    exec 9>"$config/framestride/.install.lock"
    flock -x 9
    case ${1:-} in
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
        *) echo 'Usage: treadmill.sh enable|disable' >&2; return 2;;
    esac
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then main "$@"; fi

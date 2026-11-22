#!/usr/bin/env bash
set -euo pipefail

data=${XDG_DATA_HOME:-$HOME/.local/share}
config=${XDG_CONFIG_HOME:-$HOME/.config}
root=$data/framestride
unit=framestride.service
service=$config/systemd/user/$unit
autostart=$config/systemd/user/default.target.wants/$unit
uid=$(id -u)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$uid}
if [[ -S $XDG_RUNTIME_DIR/bus ]]; then export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus; fi
if [[ $uid == 0 ]]; then echo 'Uninstall as the original user.' >&2; exit 1; fi
if [[ -d $config/framestride ]]; then
    exec 9>"$config/framestride/.install.lock"
    flock -x 9
fi
if [[ -L $root || ( -e $root && ! -f $root/.framestride-install ) ]]; then
    echo "Cannot remove an unrecognised directory at $root." >&2; exit 1
fi

if [[ -f $root/.framestride-install ]]; then
    legacy=false
    if [[ -L $autostart ]] || { [[ -f $service ]] && grep -qx 'WantedBy=default.target' "$service"; }; then
        legacy=true
        systemctl --user disable --now "$unit"
    elif [[ -f $service ]]; then
        systemctl --user stop "$unit"
    fi
    if $legacy || [[ -f $root/.treadmill-registered ]]; then
        (source "$root/treadmill.sh"; registration removedriver)
        echo 'Restart SteamVR to unload the FrameStride driver.'
    fi
    "$root/framestride" --unregister >/dev/null 2>&1 || true
    rm -rf -- "$root"
fi
rm -f -- "$service" "$config/openxr/1/api_layers/implicit.d/framestride.json" "$data/applications/framestride.desktop"
systemctl --user daemon-reload 2>/dev/null || true
printf 'FrameStride removed. Settings and calibration are in %s/framestride/.\n' "$config"

#!/usr/bin/env bash
set -euo pipefail

data=${XDG_DATA_HOME:-$HOME/.local/share}
config=${XDG_CONFIG_HOME:-$HOME/.config}
root=$data/framestride
unit=framestride.service
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
if [[ $(id -u) == 0 ]]; then echo 'Uninstall as the original user.' >&2; exit 1; fi
if [[ -d $config/framestride ]]; then
    exec 9>"$config/framestride/.install.lock"
    flock -x 9
fi
if [[ -L $root || ( -e $root && ! -f $root/.framestride-install ) ]]; then
    echo "Cannot remove an unrecognised directory at $root." >&2; exit 1
fi

if [[ -f $root/.framestride-install ]]; then
    if [[ -f $config/systemd/user/$unit ]] && ! systemctl --user disable --now "$unit"; then
        echo 'Unable to stop the FrameStride user service. Rerun in the original SteamOS user session.' >&2; exit 1
    fi
    driver=$root/driver/framestride
    registrar=''
    for candidate in "${FRAMESTRIDE_VRPATHREG:-}" /opt/steamvr/bin/vrpathreg.sh \
        /opt/steamvr/bin/linuxarm64/vrpathreg \
        "$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/vrpathreg.sh"; do
        if [[ -x $candidate ]]; then registrar=$candidate; break; fi
    done
    if [[ -z $registrar ]]; then registrar=$(command -v vrpathreg || true); fi
    if [[ -z $registrar ]]; then
        echo 'Could not find vrpathreg. Restore SteamVR or set FRAMESTRIDE_VRPATHREG, then rerun.' >&2; exit 1
    fi
    if ! "$registrar" removedriver "$driver"; then
        registry=$("$registrar" show) || { echo 'Unable to verify driver removal.' >&2; exit 1; }
        if printf '%s\n' "$registry" | sed 's/^[[:space:]]*//' | grep -Fx -- "$driver" >/dev/null; then
            echo 'Could not unregister the FrameStride driver.' >&2; exit 1
        fi
    fi
    "$root/framestride" --unregister >/dev/null 2>&1 || true
    rm -rf -- "$root"
fi
rm -f -- "$config/systemd/user/$unit" "$config/openxr/1/api_layers/implicit.d/framestride.json"
systemctl --user daemon-reload 2>/dev/null || true
printf 'FrameStride removed. Settings and calibration are in %s/framestride/.\n' "$config"

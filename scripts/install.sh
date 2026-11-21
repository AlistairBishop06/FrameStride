#!/usr/bin/env bash
set -euo pipefail
umask 022

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
data=${XDG_DATA_HOME:-$HOME/.local/share}
config=${XDG_CONFIG_HOME:-$HOME/.config}
root=$data/framestride
service=$config/systemd/user/framestride.service
xrdir=$config/openxr/1/api_layers/implicit.d
xrmanifest=$xrdir/framestride.json
unit=framestride.service
uid=$(id -u)
runtime_dir=${XDG_RUNTIME_DIR:-/run/user/$uid}
if [[ ! -S $runtime_dir/bus && -S /run/user/$uid/bus ]]; then runtime_dir=/run/user/$uid; fi
export XDG_RUNTIME_DIR=$runtime_dir
if [[ -S $runtime_dir/bus ]]; then export DBUS_SESSION_BUS_ADDRESS=unix:path=$runtime_dir/bus; fi

if [[ $(uname -s) != Linux || $(uname -m) != aarch64 ]]; then
    echo 'This package is for Linux aarch64.' >&2; exit 1
fi
if [[ $uid == 0 ]]; then
    echo 'Run the installer as your regular Steam Frame user.' >&2; exit 1
fi
if [[ ! -x $here/payload/framestride || ! -f $here/payload/driver/framestride/bin/linuxarm64/driver_framestride.so ]]; then
    echo 'Extract the complete release archive before installing.' >&2; exit 1
fi
case "$root:$config" in
    *$'\n'*|*$'\r'*|*$'\t'*) echo 'Install paths cannot contain control whitespace.' >&2; exit 1;;
esac

find_registrar() {
    local candidate
    for candidate in "${FRAMESTRIDE_VRPATHREG:-}" /opt/steamvr/bin/vrpathreg.sh \
        /opt/steamvr/bin/linuxarm64/vrpathreg \
        "$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/vrpathreg.sh"; do
        if [[ -x $candidate ]]; then printf '%s\n' "$candidate"; return 0; fi
    done
    command -v vrpathreg
}
registrar=$(find_registrar) || { echo 'SteamVR driver registration tool could not be located.' >&2; exit 1; }
systemctl --user show-environment >/dev/null || { echo 'Open Konsole in the logged-in SteamOS session as steamos and rerun the installer.' >&2; exit 1; }

mkdir -p -- "$data" "$config/framestride" "$(dirname "$service")" "$xrdir"
exec 9>"$config/framestride/.install.lock"
flock -x 9
if [[ -e $root && ! -f $root/.framestride-install ]]; then
    echo "Cannot use $root because it is not a FrameStride installation." >&2; exit 1
fi
if [[ -L $root ]]; then echo "Invalid installation path $root" >&2; exit 1; fi
staging=$(mktemp -d "$data/.framestride-install.XXXXXXXX")
old=''
replaced=false
registered=false
complete=false
cleanup() {
    local status=$?
    if $replaced && ! $complete; then
        if [[ -n $old ]]; then
            systemctl --user stop "$unit" 2>/dev/null || true
        else
            systemctl --user disable --now "$unit" 2>/dev/null || true
        fi
        if [[ -z $old ]] && $registered; then
            "$registrar" removedriver "$root/driver/framestride" 2>/dev/null || true
        fi
        rm -rf -- "$root"
        if [[ -n $old ]]; then mv -- "$old" "$root"; fi
        for file in "$service" "$xrmanifest"; do
            if [[ -e $staging/previous/${file##*/} ]]; then
                mv -f -- "$staging/previous/${file##*/}" "$file"
            else
                rm -f -- "$file"
            fi
            rm -f -- "$file.tmp"
        done
        systemctl --user daemon-reload 2>/dev/null || true
        if [[ -n $old && -f $service ]]; then systemctl --user start "$unit" 2>/dev/null || true; fi
    fi
    rm -rf -- "$staging"
    return "$status"
}
trap cleanup EXIT
mkdir "$staging/app" "$staging/previous"
for file in "$service" "$xrmanifest"; do
    if [[ -e $file ]]; then cp -a -- "$file" "$staging/previous/"; fi
done
stageapp=$staging/app
cp -a -- "$here/payload/." "$stageapp/"
install -m 755 "$here/uninstall.sh" "$here/test-on-frame.sh" "$stageapp/"
install -m 644 "$here/README.md" "$here/LICENSE" "$stageapp/"
install -Dm 644 "$here/docs/INPUT.md" "$stageapp/docs/INPUT.md"
cp -a -- "$here/licenses" "$stageapp/"
touch "$stageapp/.framestride-install"
if [[ -f $service ]]; then systemctl --user stop "$unit"; fi
if [[ -d $root ]]; then
    old=$(mktemp -d "$data/.framestride-previous.XXXXXXXX")
    rmdir -- "$old"
    mv -- "$root" "$old"
fi
if ! mv -- "$stageapp" "$root"; then
    if [[ -n $old ]]; then mv -- "$old" "$root"; systemctl --user start "$unit" || true; fi
    exit 1
fi
replaced=true
if [[ ! -e $config/framestride/settings.json ]]; then
    install -m 600 "$root/default-settings.json" "$config/framestride/settings.json"
fi

if "$registrar" adddriver "$root/driver/framestride"; then
    registered=true
else
    if ! "$registrar" show 2>/dev/null | sed 's/^[[:space:]]*//' | grep -Fx -- "$root/driver/framestride" >/dev/null; then
        echo 'SteamVR driver registration failed.' >&2
        exit 1
    fi
fi

escaped=${root//\\/\\\\}
escaped=${escaped//\"/\\\"}
cat >"$xrmanifest.tmp" <<EOF
{
  "file_format_version": "1.0.0",
  "api_layer": {
    "name": "XR_APILAYER_FRAMESTRIDE_locomotion",
    "library_path": "$escaped/libframestride_openxr.so",
    "api_version": "1.0",
    "implementation_version": "1",
    "description": "FrameStride local walk-in-place locomotion",
    "enable_environment": "FRAMESTRIDE_XR_ENABLE",
    "disable_environment": "FRAMESTRIDE_XR_DISABLE"
  }
}
EOF
mv -f -- "$xrmanifest.tmp" "$xrmanifest"
service_path=${escaped//%/%%}
service_path=${service_path//\$/\$\$}
cat >"$service.tmp" <<EOF
[Unit]
Description=FrameStride SteamVR dashboard and locomotion engine
StartLimitIntervalSec=0

[Service]
Type=simple
ExecStart="$service_path/framestride"
Restart=on-failure
RestartSec=8
TimeoutStopSec=2

[Install]
WantedBy=default.target
EOF
mv -f -- "$service.tmp" "$service"
systemctl --user daemon-reload
systemctl --user enable --now "$unit"
complete=true
if [[ -n $old ]]; then rm -rf -- "$old"; fi
printf '\nInstalled FrameStride in %s\n' "$root"
echo 'Restart SteamVR so the driver loads, then open FrameStride in its dashboard.'

#!/usr/bin/env bash
set -euo pipefail

archive=$(realpath "${1:?supply the ARM64 archive}")
bootstrap=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../scripts" && pwd -P)/install-from-release.sh
sandbox=$(mktemp -d)
trap 'rm -rf -- "$sandbox"' EXIT
tar -xzf "$archive" -C "$sandbox" --strip-components=1
mkdir "$sandbox/bin" "$sandbox/runtime"
export HOME="$sandbox/test home"
export XDG_CONFIG_HOME="$HOME/config"
export XDG_DATA_HOME="$HOME/local share"
export XDG_RUNTIME_DIR="$sandbox/runtime"
export FRAMESTRIDE_VRPATHREG="$sandbox/bin/vrpathreg"
export TEST_DRIVER_REGISTRY="$sandbox/registered"
export TEST_ARCHIVE=$archive
export TEST_VERSION=${archive##*/framestride-}
TEST_VERSION=${TEST_VERSION%-linux-aarch64.tar.gz}
export PATH="$sandbox/bin:$PATH"
cat >"$sandbox/bin/uname" <<'EOF'
#!/usr/bin/env bash
if [[ ${1:-} == -m ]]; then echo aarch64; else echo Linux; fi
EOF
cat >"$sandbox/bin/id" <<'EOF'
#!/usr/bin/env bash
if [[ ${1:-} == -u ]]; then echo 1000; else exit 2; fi
EOF
cat >"$sandbox/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
set -eu
[[ $1 == --user ]]
case "$2" in
    show-environment|stop) exit 0;;
    daemon-reload) [[ ${FRAMESTRIDE_TEST_SERVICE_FAIL:-0} != 1 ]];;
    disable)
        [[ $3 == --now && $4 == framestride.service ]]
        rm -f -- "$XDG_CONFIG_HOME/systemd/user/default.target.wants/$4";;
    *) exit 2;;
esac
EOF
cat >"$sandbox/bin/vrpathreg" <<'EOF'
#!/usr/bin/env bash
set -eu
case "$1" in
    adddriver)
        if grep -Fxq -- "$2" "$TEST_DRIVER_REGISTRY"; then exit 1; fi
        [[ -f "$2/driver.vrdrivermanifest" ]]
        printf '%s\n' "$2" >> "$TEST_DRIVER_REGISTRY";;
    removedriver)
        if ! grep -Fxq -- "$2" "$TEST_DRIVER_REGISTRY"; then exit 1; fi
        grep -Fvx -- "$2" "$TEST_DRIVER_REGISTRY" > "$TEST_DRIVER_REGISTRY.tmp" || true
        mv "$TEST_DRIVER_REGISTRY.tmp" "$TEST_DRIVER_REGISTRY";;
    show) sed 's/^/\t[driver] /' "$TEST_DRIVER_REGISTRY";;
    *) exit 2;;
esac
EOF
cat >"$sandbox/bin/curl" <<'EOF'
#!/usr/bin/env bash
set -eu
[[ ${@: -3:1} == --output ]]
output=${@: -2:1}
url=${@: -1}
base=https://github.com/AlistairBishop06/FrameStride/releases
case "$url" in
    "$base/latest/download/SHA256SUMS"|"$base/download/v$TEST_VERSION/SHA256SUMS")
        cp "${TEST_ARCHIVE%/*}/SHA256SUMS" "$output";;
    "$base/download/v$TEST_VERSION/${TEST_ARCHIVE##*/}")
        if [[ ${FRAMESTRIDE_TEST_CORRUPT:-0} == 1 ]]; then printf 'corrupt archive' > "$output"; else cp "$TEST_ARCHIVE" "$output"; fi;;
    *) echo "Unexpected download $url" >&2; exit 2;;
esac
EOF
chmod +x "$sandbox/bin/"*
printf '%s\n' '/unrelated/existing-driver' > "$TEST_DRIVER_REGISTRY"
mkdir -p -- "$XDG_CONFIG_HOME/systemd/user" "$XDG_DATA_HOME"
touch "$XDG_CONFIG_HOME/systemd/user/unrelated.service" "$XDG_DATA_HOME/unrelated"
if FRAMESTRIDE_TEST_CORRUPT=1 bash "$bootstrap"; then
    echo 'Release installer accepted a corrupt download' >&2; exit 1
fi
[[ ! -e "$XDG_DATA_HOME/framestride" ]]
cat "$bootstrap" | bash
unit=$XDG_CONFIG_HOME/systemd/user/framestride.service
autostart=$XDG_CONFIG_HOME/systemd/user/default.target.wants/framestride.service
launcher=$XDG_DATA_HOME/applications/framestride.desktop
driver=$XDG_DATA_HOME/framestride/driver/framestride
[[ $(cat "$TEST_DRIVER_REGISTRY") == '/unrelated/existing-driver' ]]
[[ ! -L $autostart ]]
grep -Fx 'After=steamvr.service' "$unit"
grep -Fx 'Requisite=steamvr.service' "$unit"
if grep -Eq '^(WantedBy|Restart)=' "$unit"; then echo 'FrameStride can start without a user request' >&2; exit 1; fi
grep -Fx 'Exec=systemctl --user start framestride.service' "$launcher"
cmp "$XDG_CONFIG_HOME/framestride/settings.json" "$sandbox/payload/default-settings.json"
printf '%s\n' '{"schema":1,"calibrated":true,"walk_cadence_hz":1.77}' > "$XDG_CONFIG_HOME/framestride/settings.json"
cp "$XDG_CONFIG_HOME/framestride/settings.json" "$sandbox/expected-settings"
# An upgrade from 1.0.0 removes the old enablement link and driver registration.
mkdir -p -- "${autostart%/*}"
ln -s -- "$unit" "$autostart"
printf '\n[Install]\nWantedBy=default.target\n' >> "$unit"
printf '%s\n' "$driver" >> "$TEST_DRIVER_REGISTRY"
bash "$bootstrap" "v$TEST_VERSION"
[[ ! -L $autostart && $(cat "$TEST_DRIVER_REGISTRY") == '/unrelated/existing-driver' ]]
"$sandbox/install.sh"
cmp "$XDG_CONFIG_HOME/framestride/settings.json" "$sandbox/expected-settings"
[[ $(wc -l < "$TEST_DRIVER_REGISTRY") == 1 ]]
[[ -x "$XDG_DATA_HOME/framestride/framestride" ]]
touch "$XDG_DATA_HOME/framestride/previous-version-marker"
if FRAMESTRIDE_TEST_SERVICE_FAIL=1 "$sandbox/update.sh"; then
    echo 'Update should roll back if systemd cannot reload the new unit' >&2; exit 1
fi
[[ -f "$XDG_DATA_HOME/framestride/previous-version-marker" ]]
cmp "$XDG_CONFIG_HOME/framestride/settings.json" "$sandbox/expected-settings"
python3 - "$XDG_CONFIG_HOME/openxr/1/api_layers/implicit.d/framestride.json" <<'PY'
import json
from pathlib import Path
import sys

data = json.loads(Path(sys.argv[1]).read_text())['api_layer']
assert data['enable_environment'] == 'FRAMESTRIDE_XR_ENABLE'
assert Path(data['library_path']).is_file()
PY
"$XDG_DATA_HOME/framestride/treadmill.sh" enable
"$XDG_DATA_HOME/framestride/treadmill.sh" enable
[[ -f "$XDG_DATA_HOME/framestride/.treadmill-registered" && $(wc -l < "$TEST_DRIVER_REGISTRY") == 2 ]]
"$XDG_DATA_HOME/framestride/treadmill.sh" disable
"$XDG_DATA_HOME/framestride/treadmill.sh" disable
[[ ! -e "$XDG_DATA_HOME/framestride/.treadmill-registered" && $(wc -l < "$TEST_DRIVER_REGISTRY") == 1 ]]
"$XDG_DATA_HOME/framestride/treadmill.sh" enable
"$XDG_DATA_HOME/framestride/uninstall.sh"
"$sandbox/uninstall.sh"
cmp "$XDG_CONFIG_HOME/framestride/settings.json" "$sandbox/expected-settings"
[[ ! -e "$XDG_DATA_HOME/framestride" && ! -e "$XDG_CONFIG_HOME/systemd/user/framestride.service" ]]
[[ ! -e "$XDG_CONFIG_HOME/openxr/1/api_layers/implicit.d/framestride.json" ]]
[[ ! -e $launcher ]]
[[ -e "$XDG_CONFIG_HOME/systemd/user/unrelated.service" && -e "$XDG_DATA_HOME/unrelated" ]]
[[ $(cat "$TEST_DRIVER_REGISTRY") == '/unrelated/existing-driver' ]]
echo 'PASS: manual launcher, no boot enablement or driver registration by default, legacy migration, driver opt-in, retained calibration, rollback and removal'

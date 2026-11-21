#!/usr/bin/env bash
set -euo pipefail

archive=$(realpath "${1:?supply the ARM64 archive}")
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
    show-environment|stop|start|daemon-reload) exit 0;;
    enable) [[ ${FRAMESTRIDE_TEST_SERVICE_FAIL:-0} != 1 && $3 == --now && -f "$XDG_CONFIG_HOME/systemd/user/$4" ]];;
    disable) [[ $3 == --now && $4 == framestride.service ]];;
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
    show) sed 's/^/\t/' "$TEST_DRIVER_REGISTRY";;
    *) exit 2;;
esac
EOF
chmod +x "$sandbox/bin/"*
printf '%s\n' '/unrelated/existing-driver' > "$TEST_DRIVER_REGISTRY"
mkdir -p -- "$XDG_CONFIG_HOME/systemd/user" "$XDG_DATA_HOME"
touch "$XDG_CONFIG_HOME/systemd/user/unrelated.service" "$XDG_DATA_HOME/unrelated"
"$sandbox/install.sh"
cmp "$XDG_CONFIG_HOME/framestride/settings.json" "$sandbox/payload/default-settings.json"
printf '%s\n' '{"schema":1,"calibrated":true,"walk_cadence_hz":1.77}' > "$XDG_CONFIG_HOME/framestride/settings.json"
cp "$XDG_CONFIG_HOME/framestride/settings.json" "$sandbox/expected-settings"
"$sandbox/update.sh"
"$sandbox/install.sh"
cmp "$XDG_CONFIG_HOME/framestride/settings.json" "$sandbox/expected-settings"
[[ $(wc -l < "$TEST_DRIVER_REGISTRY") == 2 ]]
[[ -x "$XDG_DATA_HOME/framestride/framestride" ]]
touch "$XDG_DATA_HOME/framestride/previous-version-marker"
if FRAMESTRIDE_TEST_SERVICE_FAIL=1 "$sandbox/update.sh"; then
    echo 'Update should fail if the service cannot start' >&2; exit 1
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
"$XDG_DATA_HOME/framestride/uninstall.sh"
"$sandbox/uninstall.sh"
cmp "$XDG_CONFIG_HOME/framestride/settings.json" "$sandbox/expected-settings"
[[ ! -e "$XDG_DATA_HOME/framestride" && ! -e "$XDG_CONFIG_HOME/systemd/user/framestride.service" ]]
[[ ! -e "$XDG_CONFIG_HOME/openxr/1/api_layers/implicit.d/framestride.json" ]]
[[ -e "$XDG_CONFIG_HOME/systemd/user/unrelated.service" && -e "$XDG_DATA_HOME/unrelated" ]]
[[ $(cat "$TEST_DRIVER_REGISTRY") == '/unrelated/existing-driver' ]]
echo 'PASS: clean install, repeated installation, update, rollback, retained calibration, repeated uninstall, and unrelated files untouched'

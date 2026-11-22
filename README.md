# FrameStride

FrameStride converts rhythmic headset movement into continuously variable forward input for smooth-locomotion VR games. It samples the Steam Frame headset pose at 80 Hz, estimates step cadence and amplitude, and maps that motion to analogue output using your calibration. It uses the headset's existing tracking. The dashboard runs inside SteamVR and accepts controller laser-pointer input.

**Version 1.0.2 launches on demand from the Steam Frame dashboard.** Installation adds a shortcut and leaves the optional SteamVR treadmill driver unregistered. The previous automatic startup is removed during upgrade. The manual-launch build still requires on-device validation. See the [input guide](docs/INPUT.md) for the two input routes and their coverage.

## Install on the Frame over RDP

Enable Developer Mode, connect to the Frame over Remote Desktop, and open **Konsole** in the `steamos` session. Run this command as `steamos` without `sudo`. It downloads the latest published ARM64 build from GitHub Releases, checks its SHA-256, and installs it on the headset. [Valve’s Remote Desktop instructions](https://partner.steamgames.com/doc/steamhardware/steamframe/debugging)

```sh
curl -fsSL https://github.com/AlistairBishop06/FrameStride/releases/latest/download/install-framestride.sh | bash
```

Return to the headset’s dashboard and select **+ → Launch Application → FrameStride**. The app waits for you to launch it each session. Run the same command to update. Your calibration and settings carry over. If the installer removes a previously registered treadmill driver, restart SteamVR to unload it.

## Other installation methods

### Windows SSH installer

On the Frame, turn on **Steam Settings → System → Enable Developer Mode** and set a user password in **Developer**. The Windows PC and headset need to be reachable on the same network. The installer uses the Windows OpenSSH `ssh.exe` and `scp.exe` clients. [Valve’s Steam Frame setup guide](https://partner.steamgames.com/doc/steamhardware/steamframe/setup)

Download `framestride-1.0.2-linux-aarch64.tar.gz` and `SHA256SUMS` from the release. Compare the archive hash with `Get-FileHash -Algorithm SHA256 .\framestride-1.0.2-linux-aarch64.tar.gz`. Extract the archive and run the installer from the directory holding both the archive and the extracted folder.

```powershell
tar -xzf .\framestride-1.0.2-linux-aarch64.tar.gz
powershell -NoProfile -ExecutionPolicy Bypass -File .\framestride-1.0.2-linux-aarch64\install.ps1
```

Pass `-Target <Frame IPv4 address>` if the name `frame` fails. The script uploads the archive and runs the Linux installer as `steamos`. Open FrameStride from **+ → Launch Application** in the headset.

### Install from a downloaded archive

Enable Developer Mode and open a terminal on the Frame or SSH to it as `steamos`. Download or copy the archive to the headset and run the following commands. The installer adds the dashboard launcher, its on-demand user service, and the opt-in OpenXR manifest. Files go under `~/.local/share/framestride` and `~/.config` by default.

```sh
sha256sum -c SHA256SUMS
tar -xzf framestride-1.0.2-linux-aarch64.tar.gz
cd framestride-1.0.2-linux-aarch64
./install.sh
```

Launch FrameStride from the dashboard’s **+** menu with SteamVR running. **Settings → Stop application** stops it for the current session. The shortcut can launch it again.

## Calibrate and test

1. Open **Tracking → Start calibration**. Stand still for five seconds and walk normally in place for twelve. Jog for seven seconds or choose **Skip jogging** to save walking calibration with an estimated jogging range. Keep a clear play space and face forward. The wizard keeps synthetic output at neutral while you calibrate.
2. Check the **Live stick preview**. You can repeat calibration while using the previous saved calibration if a new run fails. **Movement** controls sensitivity, speeds, the response curve, smoothing and the stop limit. **Input Test** shows the live headset signal, steps, output submitted by the backend and the age of its last update.
3. Choose a game input route. SteamVR Input requires explicitly registering the optional treadmill driver and restarting SteamVR. The OpenXR layer is enabled per compatible native game. [Driver setup, binding steps and OpenXR configuration](docs/INPUT.md)
4. In **Overview**, select **Enable locomotion** and close the dashboard. Walk, stop, then test physical backwards movement and strafing. Confirm the character reacts to each test. The **Emergency stop** button disarms locomotion from any tab. With the default settings, opening the SteamVR dashboard also pauses output.

Where the runtime reports grip and stick clicks through the legacy controller state, you can enable the emergency shortcut in **Settings** and hold the left grip plus left stick click for 600 ms. Check the shortcut with output at neutral before relying on it.

For a backend check in Konsole or over SSH, run the following command with the headset awake and the controllers connected. Add `--probe` to read the actual SteamVR Input Test action for up to 25 seconds. For an isolated run, temporarily turn off **Only enable output with a game running**, select the SteamVR treadmill backend, close the dashboard and walk briefly. A successful readback is evidence for the probe's binding. Verify each game separately.

```sh
~/.local/share/framestride/test-on-frame.sh --probe
```

## Update and uninstall

Rerun the release command in Konsole for the latest published version. For a specific release, pass its tag to the installer, for example `| bash -s -- v1.0.2` at the end of the command. Installation clears a previous treadmill registration. For treadmill testing after an update, register it again and restart SteamVR. Settings and calibration are stored in `~/.config/framestride/settings.json` and persist across updates and uninstallations.

```sh
~/.local/share/framestride/uninstall.sh
```

Restart SteamVR after uninstalling if you registered the optional treadmill driver. Remove any personal game bindings or OpenXR launch options you added during game setup.

## Troubleshooting

| Symptom | Check |
|---|---|
| Dashboard missing | Confirm SteamVR is running and open FrameStride from the **+** menu. Check the user service log for OpenVR interface and texture errors. |
| No tracking | Wake the headset and confirm SteamVR reports valid room tracking. Try **Input Test** with the headset worn. |
| Walking detected but no movement | Verify the backend is connected, the dashboard is closed, and the intended game action is bound. Run the probe and follow the input guide. |
| Driver fails to load | Restart SteamVR, check its add-on settings and look for `framestride` in the server log and `vrpathreg show`. |
| Input goes to the wrong action | Check the game’s Move action set and its treadmill binding. In OpenXR, check the exact `FRAMESTRIDE_XR_ACTION` value. |
| Physical stick stops overriding | Verify the original game binding and **Physical stick Y takes priority**. The treadmill route needs a readable left joystick for override. Try the OpenXR route if the game supports it. |
| Jogging is weak | Recalibrate, then check sensitivity and the activation threshold. The detector’s cadence window is approximately 0.8–3.2 Hz. |
| Movement continues after stopping | Open the dashboard and press **Emergency stop**. SSH can also run `systemctl --user stop framestride.service`. Check the submitted output and whether the game applies its own movement smoothing. |
| SteamOS update causes a regression | Collect the doctor output and SteamVR log. Check that SteamVR still exposes `IVRSystem_026` and `IVROverlay_028`, and that the custom add-on remains enabled. |
| PC-streamed game does not move | This package supplies a local headset backend. Windows host injection and a companion app are outside this release. |

```sh
systemctl --user status framestride.service
journalctl --user -u framestride.service -b --no-pager
~/.local/share/framestride/framestride --doctor
/opt/steamvr/bin/vrpathreg.sh show
```

Look for `FrameStride` in `vrserver.txt` in the SteamVR log directory reported by `vrpathreg show`.

## Build and verify

On a Linux development machine, install CMake 3.20 or newer, Ninja, a C++20 compiler and Python 3. CMake fetches Valve OpenVR headers, the Khronos OpenXR SDK headers, nlohmann JSON, stb_truetype and Inter using pinned versions and SHA-256. SteamVR supplies `libopenvr_api.so` on the headset.

```sh
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

For release builds, put Zig **0.14.1** on `PATH`. The toolchain targets Linux AArch64 with glibc 2.34 or newer. You can also build natively with the host commands on Linux ARM64 and stage that build.

```sh
cmake -S . -B build-frame -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/zig-linux.cmake \
  -DCMAKE_BUILD_TYPE=Release -DFRAMESTRIDE_NATIVE=ON -DBUILD_TESTING=OFF
cmake --build build-frame
cmake --install build-frame --prefix build-frame/stage
python3 scripts/package.py build-frame/stage dist
bash tests/install_test.sh dist/framestride-1.0.2-linux-aarch64.tar.gz
```

The tests use synthetic headset motion and mock OpenVR and OpenXR runtimes. The package check validates all three ELF architectures, verifies archive contents after extraction, and exercises install, update and uninstall with isolated registration and service test doubles. Measure CPU and power on a real headset with the dashboard closed and open. Headset resource usage is awaiting measurement.

```sh
ps -C framestride -o etime,%cpu,rss,args
```

Source is MIT-licensed. Third-party licence texts are included in the release.

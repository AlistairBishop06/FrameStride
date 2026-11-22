# FrameStride input and game setup

## SteamVR treadmill

The native ARM64 driver registers a controller in SteamVR’s treadmill role. It publishes an absolute two-sided joystick vector with X at zero and a forward Y proportional to walking effort. The two Frame Controllers keep their hand roles. The driver owns the treadmill input components and has no API access to write the components owned by Valve's Frame Controller driver. SteamVR resolves sources bound to the same action by magnitude, so each game and restriction path needs testing. [Valve driver API](https://github.com/ValveSoftware/openvr/blob/v2.15.6/docs/Driver_API_Documentation.md)

The standard install leaves this driver unregistered. Test the **+ → Launch Application → FrameStride** shortcut first. To opt into treadmill testing, run this in Konsole as `steamos`, restart SteamVR, and then launch FrameStride. The driver waits for the open FrameStride app before adding a virtual controller and disconnects it when FrameStride closes or switches to OpenXR.

```sh
~/.local/share/framestride/treadmill.sh enable
```

Run `~/.local/share/framestride/treadmill.sh disable` and restart SteamVR to remove the registration. A new installation or upgrade also clears an earlier registration.

### Bind a game's movement action

1. Launch the game through native SteamVR and choose **Smooth** or **Continuous Locomotion** in the game.
2. Open SteamVR **Settings → Controllers → Manage Controller Bindings**, select the game, and use **Custom → Edit This Binding**. Use **Show Old Controller Binding UI** if the treadmill device is missing from the headset UI.
3. Select **FrameStride Treadmill**, add its joystick as **Joystick**, and bind **Position** to the game’s vector2 Move, Walk or Locomotion action in the action set that is active during play. If the game restricts the action to the left-hand device, enable the binding editor option to also return the treadmill binding with that hand, where available. Save it as a personal binding.
4. Keep the original hand-controller binding. Enable FrameStride, close the dashboard, and start walking. Check forward movement, coast to neutral, physical backwards input and simultaneous walking with left-stick X deflected. Reset the game’s personal binding to return it to the game default.

The package includes `payload/probe/treadmill-binding.json` and `payload/probe/standard-locomotion-template.json`. The first binds a known action for the shipped Input Test. The second shows the exact binding source with placeholders for a game’s application key and action paths. Use the game’s actual action manifest and SteamVR editor to create the personal binding.

**Standard Locomotion** keeps the physical stick X and other controls available through the existing controller bindings. With physical override enabled, the detector suppresses the treadmill source when the physical left stick Y reaches the configured threshold, in either direction. SteamVR’s action arbitration can select one vector source over another. A treadmill binding can therefore lose simultaneous X plus synthetic Y in a game that samples a single vector or only accepts the hand origin. If the overlay cannot read a physical left joystick through OpenVR, it blocks synthetic treadmill output while override is enabled. The dashboard reports this condition. A user can deliberately turn off the override when testing a title that has no accessible joystick state.

The actual input proof has three stages.

| Check | What it establishes |
|---|---|
| Preview or graph moves | SteamVR pose tracking and the motion detector produced an output. |
| Input Test shows the driver registered and submitted | The driver created the scalar components and SteamVR accepted updates. |
| `test-on-frame.sh --probe` reads nonzero followed by zero | A separate OpenVR application received the SteamVR action through its bundled binding. |
| The character moves and stops in a specific game | The game's binding and locomotion consume the input in that mode. |

## OpenXR action layer

Use this route for an ARM64 Linux OpenXR title that exposes a named vector2 Move action bound to the physical left thumbstick. It uses the standard OpenXR API-layer loader to pass the real X through and replace Y only for the named action. Physical Y at or above the override threshold wins. Other actions and controller tracking continue through the runtime unchanged. The layer requires a successfully synced focused session, an active action and a valid left-stick binding. An aggregate query using multiple inputs passes through. [Khronos OpenXR API layer loader](https://registry.khronos.org/OpenXR/specs/1.1/loader.html)

1. Find the game’s actual OpenXR action name, for example `move`. Use the name supplied to `xrCreateAction`, with matching case. A generic SteamVR binding name may be different.
2. Set the game’s Steam launch options to `FRAMESTRIDE_XR_ENABLE=1 FRAMESTRIDE_XR_ACTION=move %command%`. Select **Use the opt-in OpenXR action layer** in the FrameStride settings.
3. Open the game and let it receive controller input. Return to the FrameStride dashboard to enable locomotion after it shows the backend connected. Close the dashboard and check locomotion, strafing, manual override and stopping. Remove the launch options to disable loading the layer in that game.

The installed API layer library targets native Linux ARM64. Proton/FEX and Lepton use different loader or process environments and require their own compatible packaging and tests. An OpenVR title cannot consume an OpenXR API layer. ARM64 OpenXR and SteamVR treadmill paths are both awaiting on-headset validation.

## Local gamepads and streamed titles

Linux `uinput` creates an independent virtual input device. Steam Frame maps non-VR titles through Steam Input, and a virtual gamepad does not provide the tracked left-hand action that VR games read. FrameStride does not install a `uinput` device or request permission to access one. [Linux uinput API](https://docs.kernel.org/input/uinput.html) [Valve Steam Frame input](https://partner.steamgames.com/doc/steamhardware/steamframe/input)

FrameStride has no verified route for forwarding a headset-created treadmill or gamepad to a Windows SteamVR session. Streamed PCVR needs an independent host-side input path. For example, a paired host companion could receive timestamped motion over the LAN and drive a host OpenVR treadmill binding with its own watchdog. A virtual XInput controller would only help games that read gamepad movement and would need its own supported Windows driver. Neither host route is shipped.

## Release and safety checks on hardware

Run the doctor and probe with the dashboard closed for the walking portion. Test a still stance, normal walking, jogging and deliberate stop. Open the dashboard and check that the default pause takes effect. With the treadmill probe still active and reporting movement, stop the service or terminate the motion process and verify that Y reaches zero. The driver watchdog expires a motion heartbeat after 120 ms and submits neutral on the next SteamVR driver frame. If SteamVR itself stops making frames or the game retains a previously cached value, the check must be repeated after runtime recovery. Also check that physical buttons, triggers, grip, tracking and hand roles work before and after the test.

The developer examples [Frame Perf Overlay](https://github.com/sasaken1102r/frame-perf-overlay), [Frame Handtracking](https://github.com/Olivphil/frame_handtracking) and [Frame Control](https://github.com/saphid/frame-control) informed the runtime location, native driver layout and SSH model. FrameStride uses its own application and driver code.

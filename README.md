# BurnoutAssist

**A lightweight GTA IV ASI plugin for wheelspin-focused burnout launches.**

BurnoutAssist adds a simple, configurable way to get more aggressive launches in *Grand Theft Auto IV*. While the activation combo is held, the plugin boosts the game's native vehicle drive scalar to help produce wheelspin and a harder launch.

## What It Does

- Adds a configurable burnout/launch assist to GTA IV.
- Uses a keyboard combination to activate the assist while driving.
- Boosts the game's native drive scalar instead of applying a scripted force to the vehicle.
- Leaves the game's automatic gearbox and normal clutch behavior alone.
- Supports the game's control mapping for the trigger key when the mapping is available.
- Keeps logging quiet by default.

BurnoutAssist is intended to enhance the feel of a launch, not to replace GTA IV's vehicle physics or implement a separate transmission system.

## How It Works

BurnoutAssist is a native 32-bit ASI plugin for the supported GTA IV executable. It installs a guarded call-site hook and increases the native vehicle drive scalar only while the activation conditions are met.

The hook checks the expected instruction and target before patching, helping prevent the plugin from modifying an unexpected code location. The plugin also performs a strict executable-version check.

The assist activates while **native throttle input + mapped trigger key** are held together. When the game's control mapping can be read, the trigger follows the key assigned to the plugin's mapped in-vehicle control. If that mapping cannot be read, BurnoutAssist falls back to `TriggerKey` in the INI file.

## Installation

1. Download the release ZIP from the GitHub repository.
2. Extract `BurnoutAssist.asi` from the ZIP.
3. Copy `BurnoutAssist.asi` into your GTA IV `plugins` folder.
4. Launch the game.

If your GTA IV installation does not already have a `plugins` folder, make sure your ASI loader setup supports the folder before installing the plugin.

On first launch, BurnoutAssist creates `BurnoutAssist.ini` beside the plugin if the INI file does not already exist. Existing INI files are preserved.

## Configuration

Open `BurnoutAssist.ini` in a text editor. The file is created with defaults on first run.

```ini
[BurnoutAssist]
Enabled=1
ForceMultiplier=2.35
TriggerKey=SHIFT
Debug=0
```

| Setting | Description |
|---|---|
| `Enabled` | Enables (`1`) or disables (`0`) the assist. |
| `ForceMultiplier` | Controls how strongly the native drive scalar is boosted while the assist is active. Higher values can produce more wheelspin and more aggressive launches. |
| `TriggerKey` | Fallback trigger key used if the game's mapped control cannot be read. The in-game mapping takes priority when available. |
| `Debug` | Enables (`1`) or disables (`0`) additional debug output. Leave it at `0` for normal use. |

The activation combination is **native throttle input + mapped trigger key**. The trigger key follows the game's mapping when available; `TriggerKey` is a fallback, not necessarily the key that will be used in that case.

Start with the default multiplier and adjust it gradually. The result varies by vehicle and driving conditions.

## Important Notes

- **Supported executable:** GTA IV Complete Edition **1.2.0.59, 32-bit (x86)**.
- The plugin is version-specific. Other game builds are not supported unless explicitly confirmed.
- BurnoutAssist does not add a new gearbox, manually control clutch timing, or force a fixed-duration launch.
- Results vary between vehicles. A multiplier that feels good on one car may be too aggressive on another.
- Higher multipliers can cause excessive wheelspin and may make a vehicle harder to control.
- Do not edit the executable or move the hook to another address unless you know exactly what you are changing.
- If the game or plugin reports a version or hook-validation failure, do not force the plugin to run on that executable.
- Keep `Debug=0` unless you are troubleshooting.

## Compatibility

BurnoutAssist targets the 32-bit GTA IV Complete Edition executable version **1.2.0.59**. Compatibility with other versions, third-party patches, or conflicting ASI plugins has not been confirmed.

## Credits

Created for GTA IV players who want a more aggressive, wheelspin-focused launch without replacing the game's native vehicle behavior.

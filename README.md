# Gesture Engine

A lightweight Windows app that adds mouse gesture recognition to any mouse with a programmable button. Hold a trigger key, move the mouse in a direction, and release to execute an action — similar to the gesture system in Logitech MX Master mice, but hardware-agnostic and fully configurable.

Built as a single native C executable (~160 KB, zero runtime dependencies). Originally designed for the Razer Basilisk V3 Pro side button mapped to F24.

## Features

- **Directional gestures** — Hold trigger + move up/down/left/right to fire actions
- **Tap action** — Quick press without movement triggers a separate action (default: Ctrl+Click)
- **Per-app overrides** — Different gesture bindings per application with fallback to defaults
- **30+ built-in actions** — Navigation, window management, media controls, system shortcuts
- **Custom key sequences** — Bind any key combo, including multi-step sequences (`ctrl+k,ctrl+c`)
- **Launch programs** — Use `run:calc` syntax to launch executables from a gesture
- **Horizontal scroll** — Mouse wheel while holding trigger converts to horizontal scroll
- **Settings UI** — Full graphical settings dialog accessible from the system tray
- **Startup option** — Optional launch-on-startup via Windows registry
- **No admin required** — Runs entirely in user space

## Quick Start

### Option 1: Download (recommended)

1. Download `gesture_engine.exe` from the [latest release](https://github.com/jkwcurtis/gesture-engine/releases/latest)
2. Place it in any folder (e.g., `C:\Tools\GestureEngine\`)
3. Double-click to run — a tray icon appears in the notification area
4. Right-click the tray icon → **Settings** to configure

### Option 2: Build from Source

Requires GCC (MinGW-w64). Install it with:

```
winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
```

Then:

```
git clone https://github.com/jkwcurtis/gesture-engine.git
cd gesture-engine
build.bat
```

This produces `gesture_engine.exe` in the project root.

## Usage

1. **Map a mouse button to F24** using your mouse's software (Razer Synapse, Logitech Options, etc.)
2. **Run `gesture_engine.exe`** — it sits in the system tray
3. **Hold F24 + move the mouse** in any cardinal direction, then release
4. The configured action fires immediately on release

**Tap** (press and release F24 without moving): executes the tap action (default: Ctrl+Left Click).

**Scroll** (scroll wheel while holding F24): sends horizontal scroll events.

### Settings

Right-click the tray icon → **Settings** to open the configuration dialog:

| Tab | What it controls |
|-----|-----------------|
| **General** | Trigger key, sensitivity, dead zone, scroll behavior, startup toggle |
| **Default Gestures** | Actions for each direction (applies to all apps) |
| **App Overrides** | Per-application gesture bindings that override defaults |

## Default Gestures

| Direction | Action | Shortcut |
|-----------|--------|----------|
| Up | Close Tab | `Ctrl+W` |
| Down | Refresh | `Ctrl+R` |
| Left | Previous Tab | `Ctrl+PageUp` |
| Right | Next Tab | `Ctrl+PageDown` |

## Configuration

Settings are stored in `config.json` in the same directory as the executable. The file is created automatically on first run and updated when you change settings through the UI.

```json
{
  "settings": {
    "trigger_key": "f24",
    "tap_action": "ctrl+lclick",
    "gesture_threshold": 5,
    "dead_zone": 5,
    "scroll_enabled": true,
    "scroll_multiplier": 1,
    "reverse_scroll": true,
    "post_release_lock_ms": 100,
    "launch_on_startup": true
  },
  "defaults": {
    "up": "ctrl+w",
    "down": "ctrl+r",
    "left": "ctrl+pageup",
    "right": "ctrl+pagedown"
  },
  "apps": {
    "code.exe": {
      "up": "ctrl+k,ctrl+c"
    }
  }
}
```

### Action format

| Format | Example | Description |
|--------|---------|-------------|
| Single key combo | `ctrl+w` | Modifier(s) + key pressed together |
| Multi-step sequence | `ctrl+k,ctrl+c` | Multiple combos fired in order (comma-separated) |
| Mouse button | `ctrl+lclick` | Supports `lclick`, `rclick`, `mclick` |
| Launch program | `run:calc` | Starts an executable |

### Per-app overrides

Add an entry under `"apps"` keyed by the process name (lowercase). Only the directions you specify are overridden — unset directions fall through to the defaults.

## Built-in Actions

The settings UI includes a picker with these built-in actions:

| Category | Actions |
|----------|---------|
| **Navigation** | Back, Forward, New Tab, Close Tab, Reopen Closed Tab, Next Tab, Previous Tab, Switch Application |
| **Windows** | Show Desktop, Task View, Minimize, Maximize, Snap Left/Right, Virtual Desktop Left/Right, Close Window |
| **Media** | Volume Up/Down, Mute, Play/Pause, Next/Previous Track |
| **System** | Lock Screen, Screenshot, Emoji Menu, Settings, Calculator |
| **Clipboard** | Copy, Cut, Paste, Undo, Redo, Clipboard History |
| **Mouse** | Middle Click, Double Click |

You can also type any custom key combination directly.

## Requirements

- Windows 10 or 11
- A mouse with a button that can be remapped to a keyboard key (F24 recommended)
- No administrator privileges required

## License

[MIT](LICENSE)

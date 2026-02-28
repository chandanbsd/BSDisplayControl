# Build, Setup, and Development Guide

## Prerequisites

### All Platforms

| Requirement | Version |
| --- | --- |
| Flutter SDK | Stable channel, matching Dart SDK >= 3.10.7 |
| Git | Any recent version |

Verify Flutter installation:
```bash
flutter doctor
```

### Windows

| Requirement | Notes |
| --- | --- |
| Visual Studio 2022 | With "Desktop development with C++" workload |
| Windows 10 SDK | Included with VS 2022 |
| CMake | Included with VS 2022 or install separately |

### macOS

| Requirement | Notes |
| --- | --- |
| Xcode | 14.0+ (for Swift 5.7+) |
| Xcode Command Line Tools | `xcode-select --install` |
| CocoaPods | Not required (no third-party pods) |

### Linux (Ubuntu/Debian)

| Requirement | Install Command |
| --- | --- |
| Build essentials | `sudo apt install build-essential` |
| GTK 3 dev headers | `sudo apt install libgtk-3-dev` |
| pkg-config | `sudo apt install pkg-config` |
| CMake | `sudo apt install cmake` |
| Clang (if default) | Included on modern Ubuntu |
| GCC dev libraries | `sudo apt install libstdc++-13-dev` (or whatever version matches) |
| I2C tools (optional) | `sudo apt install i2c-tools` |
| ddcutil (optional) | `sudo apt install ddcutil` |

**Note about Clang + GCC on Ubuntu:** Modern Ubuntu versions (24.04+) ship `clang++` as the default C++ compiler but only install GCC 13 development libraries. The project's CMakeLists.txt includes an auto-detection workaround that points Clang at the available GCC toolchain. If you encounter build errors about missing `<string>` or `libstdc++`, ensure at least one `libstdc++-*-dev` package is installed.

## Building

### Debug Build (All Platforms)

```bash
flutter run -d <platform>
```

Where `<platform>` is:
- `windows` -- Windows
- `macos` -- macOS
- `linux` -- Linux

Flutter automatically:
1. Runs CMake (Windows/Linux) or Xcode (macOS) to build the native runner
2. Compiles Dart code to kernel snapshots (debug) or AOT (release)
3. Bundles Flutter assets
4. Launches the application

### Release Build

```bash
flutter build <platform> --release
```

Where `<platform>` is `windows`, `macos`, or `linux`.

**Output locations:**

| Platform | Output Directory |
| --- | --- |
| Windows | `build/windows/x64/runner/Release/` |
| macOS | `build/macos/Build/Products/Release/bs_display_control.app` |
| Linux | `build/linux/x64/release/bundle/` |

### Running Tests

```bash
flutter test
```

This runs the single widget test in `test/widget_test.dart`, which verifies the app renders without crashing. Platform-specific brightness tests are not possible without hardware.

## Linux-Specific Setup

### I2C Permissions (Required for External Monitors)

External monitor brightness control requires access to `/dev/i2c-*` devices. The app handles this automatically on first launch via a `pkexec` (PolicyKit) prompt, but you can also set it up manually:

#### Automatic (App-Managed)

On first launch, the app will:
1. Detect that `/dev/i2c-*` devices are not accessible
2. Show a system password prompt via `pkexec`
3. Run `chmod 0666 /dev/i2c-*` for immediate access
4. Install a persistent udev rule for access across reboots

#### Manual Setup

If you prefer to set up permissions yourself:

```bash
# Load the i2c-dev kernel module (if not already loaded)
sudo modprobe i2c-dev

# Make it load on boot
echo "i2c-dev" | sudo tee /etc/modules-load.d/i2c-dev.conf

# Create the i2c group if it doesn't exist
sudo groupadd -f i2c

# Add your user to the i2c group
sudo usermod -aG i2c $USER

# Create persistent udev rule
echo 'KERNEL=="i2c-[0-9]*", GROUP="i2c", MODE="0666"' | \
  sudo tee /etc/udev/rules.d/99-i2c-permissions.rules

# Reload udev rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=i2c-dev

# Log out and back in (for group membership to take effect)
```

### Verifying DDC/CI Access

After setting up permissions, you can verify DDC/CI works:

```bash
# List I2C buses
ls -la /dev/i2c-*

# If ddcutil is installed, detect monitors
ddcutil detect

# Read brightness of a specific monitor
ddcutil getvcp 10 --bus <bus_number>
```

### Backlight Permissions (Built-in Displays)

Built-in laptop displays use `/sys/class/backlight/`. The brightness file may need write permission:

```bash
# Check current permissions
ls -la /sys/class/backlight/*/brightness

# If not writable, add your user to the 'video' group
sudo usermod -aG video $USER
```

## Project Configuration Files

### pubspec.yaml

The Flutter project manifest:

```yaml
name: bs_display_control
version: 1.0.0+1
environment:
  sdk: ^3.10.7
dependencies:
  flutter:
    sdk: flutter
dev_dependencies:
  flutter_test:
    sdk: flutter
  flutter_lints: ^6.0.0
```

Key points:
- **No third-party packages** -- Only Flutter SDK and lints
- **`publish_to: 'none'`** -- Not intended for pub.dev publishing
- **`uses-material-design: true`** -- Enables Material Icons font

### analysis_options.yaml

Uses the recommended Flutter lints ruleset (`flutter_lints` package). No custom rules are overridden.

## Architecture for Contributors

If you want to add a feature or fix a bug, here's where to look:

### "I want to change the UI"

All UI code is in `lib/`:
- **Layout/screens:** `lib/screens/home_screen.dart`
- **Widgets:** `lib/widgets/display_brightness_card.dart`
- **Data model:** `lib/models/display_info.dart`
- **Theme:** `lib/main.dart`

### "I want to add a new platform channel method"

1. Add the method call in `lib/services/brightness_service.dart`
2. Handle it in ALL three platform files:
   - `windows/runner/flutter_window.cpp` (C++)
   - `macos/Runner/MainFlutterWindow.swift` (Swift)
   - `linux/runner/my_application.cc` (C++)

### "I want to fix a brightness bug on a specific platform"

| Platform | File | Key Functions |
| --- | --- | --- |
| Windows | `windows/runner/flutter_window.cpp` | `MonitorEnumProc`, `GetWmiBrightness`, `SetMonitorBrightnessById` |
| macOS | `macos/Runner/MainFlutterWindow.swift` | `getDisplayBrightness`, `setBrightness`, `getIOKitBrightness` |
| Linux | `linux/runner/my_application.cc` | `DdcGetBrightness`, `DdcSetBrightness`, `EnumerateDrmDisplays` |

### "I want to add a new display data field"

1. Add the field to `DisplayInfo` in `lib/models/display_info.dart` (including `fromMap`, `toMap`, `copyWith`)
2. Return it from native code in all three platform handlers
3. Display it in the UI widget

## Troubleshooting

### Build Fails on Linux: "Cannot find -lstdc++"

```
/usr/bin/ld: cannot find -lstdc++
```

**Cause:** Clang is looking for a GCC version that isn't installed.
**Fix:** Install the GCC development libraries:
```bash
sudo apt install libstdc++-13-dev   # or whatever version is available
```

### Build Fails on Linux: "'gtk/gtk.h' not found"

**Cause:** GTK development headers not installed.
**Fix:**
```bash
sudo apt install libgtk-3-dev
```

### App Launches But Shows "No Displays Found" on Linux

**Possible causes:**
1. No `/dev/i2c-*` devices exist -- run `sudo modprobe i2c-dev`
2. I2C permissions not set up -- the app should prompt, or set up manually (see above)
3. Monitors don't support DDC/CI -- some monitors have DDC/CI disabled in their OSD menu

### Brightness Slider Moves But Monitor Doesn't Change

**Possible causes:**
1. Falling back to xrandr gamma (software-only) -- check stderr logs for `[DDC SET]` messages
2. Wrong I2C bus being used -- the app tries both detected buses, but some setups may have more
3. DDC/CI disabled on the monitor -- check monitor OSD settings for a DDC/CI option

### macOS: "Operation not permitted" Errors

**Cause:** App Sandbox is enabled.
**Fix:** Verify that both `DebugProfile.entitlements` and `Release.entitlements` have:
```xml
<key>com.apple.security.app-sandbox</key>
<false/>
```

### LSP Errors in IDE (Linux C++ Files)

The IDE may show errors in `my_application.cc`, `my_application.h`, `main.cc`, and generated plugin files. These are **false positives** -- the LSP cannot find GTK and Flutter headers because they're resolved at CMake configure time, not by the IDE's include path. The code compiles correctly via `flutter build linux`.

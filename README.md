# BSDisplayControl

A Flutter desktop application for Linux that lets you control the brightness of all connected monitors — built-in panels and external displays — from a single unified interface.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Screenshots / UI](#2-screenshots--ui)
3. [Architecture](#3-architecture)
   - 3.1 [Layer Diagram](#31-layer-diagram)
   - 3.2 [Flutter ↔ Native Bridge (Method Channel)](#32-flutter--native-bridge-method-channel)
   - 3.3 [Brightness Control Strategies](#33-brightness-control-strategies)
   - 3.4 [Display Detection Pipeline](#34-display-detection-pipeline)
   - 3.5 [Data Flow: Setting Brightness](#35-data-flow-setting-brightness)
4. [Code Walkthrough](#4-code-walkthrough)
   - 4.1 [Dart Layer](#41-dart-layer)
   - 4.2 [Native Plugin (C++)](#42-native-plugin-c)
   - 4.3 [GTK Application Shell](#43-gtk-application-shell)
   - 4.4 [Build System (CMake)](#44-build-system-cmake)
5. [Design Decisions & Justifications](#5-design-decisions--justifications)
6. [Prerequisites](#6-prerequisites)
   - 6.1 [Development Dependencies](#61-development-dependencies)
   - 6.2 [Runtime Dependencies](#62-runtime-dependencies)
7. [Building the Project](#7-building-the-project)
   - 7.1 [Development Build (flutter run)](#71-development-build-flutter-run)
   - 7.2 [Release Build](#72-release-build)
8. [Deployment](#8-deployment)
   - 8.1 [Running the Release Bundle Directly](#81-running-the-release-bundle-directly)
   - 8.2 [Building and Installing the Snap Package](#82-building-and-installing-the-snap-package)
9. [Permissions & i2c Group Setup](#9-permissions--i2c-group-setup)
10. [Troubleshooting](#10-troubleshooting)
11. [Project Structure](#11-project-structure)

---

## 1. Overview

BSDisplayControl solves a practical Linux desktop problem: external monitors expose no brightness control in the standard GNOME or KDE settings panels. Their brightness is accessible only through hardware protocols (DDC/CI over I²C) or manufacturer software that does not exist on Linux.

This application:

- Detects all connected displays automatically at startup.
- Reads the current hardware brightness from each one.
- Lets you adjust brightness with a slider or four quick-preset buttons (Dim 25 %, Half 50 %, Bright 75 %, Max 100 %).
- Works simultaneously with built-in laptop panels (via the kernel backlight subsystem) and external monitors (via DDC/CI).
- Packages as a self-contained Snap so ddcutil does not need to be installed system-wide.

---

## 2. Screenshots / UI

```
┌─────────────────────────────────────────────────┐
│  Display Control                          [↺]   │
├─────────────────────────────────────────────────┤
│  4 displays detected                            │
│                                                 │
│ ┌─────────────────────────────────────────────┐ │
│ │ 🖥  PHL 271E1                        [ 60%] │ │
│ │     External Display                        │ │
│ │  🔅 ──────────────●────────────── 🔆        │ │
│ │  [Dim]  [Half]  [●Bright]  [Max]            │ │
│ └─────────────────────────────────────────────┘ │
│                                                 │
│ ┌─────────────────────────────────────────────┐ │
│ │ 🖥  DELL U2412M                     [ 45%] │ │
│ │     External Display                        │ │
│ │  🔅 ──────────●──────────────────── 🔆      │ │
│ │  [Dim]  [●Half]  [Bright]  [Max]            │ │
│ └─────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────┘
```

The UI follows Material Design 3. The theme (light/dark) follows the system setting automatically.

---

## 3. Architecture

### 3.1 Layer Diagram

```
┌──────────────────────────────────────────────────────────────────┐
│                        DART / FLUTTER                            │
│                                                                  │
│   main.dart          HomeScreen          DisplayBrightnessCard   │
│   (app entry,        (StatefulWidget,    (StatelessWidget,       │
│    theme setup)       list + debounce)    slider + presets)      │
│                              │                                   │
│                      BrightnessService                           │
│                      (MethodChannel client)                      │
│                              │                                   │
│                    DisplayInfo  (model)                          │
└──────────────────────┬───────────────────────────────────────────┘
                       │  Flutter MethodChannel
                       │  "com.bsdisplaycontrol/brightness"
                       │  (StandardMethodCodec — binary msgpack)
┌──────────────────────▼───────────────────────────────────────────┐
│                     C++ NATIVE PLUGIN                            │
│                                                                  │
│   brightness_plugin.cc / .h                                      │
│                                                                  │
│   ┌────────────────┐  ┌───────────────┐  ┌────────────────────┐  │
│   │ Backlight      │  │  DDC/CI       │  │  xrandr            │  │
│   │ /sys/class/    │  │  (ddcutil     │  │  (X11 only,        │  │
│   │  backlight/    │  │   subprocess) │  │   Wayland skip)    │  │
│   └───────┬────────┘  └───────┬───────┘  └─────────┬──────────┘  │
└───────────┼───────────────────┼─────────────────────┼────────────┘
            │                   │                     │
┌───────────▼───────────────────▼─────────────────────▼────────────┐
│                         LINUX KERNEL / USERSPACE                 │
│                                                                  │
│  /sys/class/backlight/      /dev/i2c-N        X11 / XWayland     │
│  (ACPI backlight driver)    (I²C bus to       (RANDR gamma ramp) │
│                              monitor EEPROM)                     │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 Flutter ↔ Native Bridge (Method Channel)

Flutter's `MethodChannel` is the standard interprocess-communication mechanism for calling platform-specific code from Dart. Conceptually it works like an RPC call:

```
Dart side                               C++ side
─────────────────────────────────────────────────────────────────
channel.invokeMethod('getDisplays')
        │
        │  [binary-encoded message over shared memory]
        ▼
                                handle_method_call()
                                   strcmp(method, "getDisplays")
                                   → build FlValue list
                                   fl_method_call_respond_success()
        │
        ◀──────────────────────────────────────────────────────
List<Map> result
cast to List<DisplayInfo>
```

The channel is registered under the name `"com.bsdisplaycontrol/brightness"`. Both sides must use this exact string — a mismatch silently results in `MissingPluginException`.

Two methods are exposed:

| Method | Arguments | Return |
|--------|-----------|--------|
| `getDisplays` | none | `List<Map>` — one map per display |
| `setBrightness` | `displayId: String`, `brightness: double` | `bool` — true on success |

### 3.3 Brightness Control Strategies

The plugin implements three strategies, tried in priority order:

```
getDisplays() call
      │
      ├─► Backlight sysfs  (/sys/class/backlight/*)
      │       Works for: built-in laptop panels, some AMD APUs
      │       Mechanism: read brightness / max_brightness files
      │
      ├─► DDC/CI via ddcutil  (/dev/i2c-N)
      │       Works for: most external monitors made after ~2005
      │       Mechanism: I²C commands to monitor's MCCS firmware
      │       VCP feature code 0x10 = Luminance (brightness)
      │
      └─► xrandr gamma ramp  (X11 only, Wayland skipped)
              Works for: X11 sessions without DDC-capable hardware
              Mechanism: CRTC gamma multiplier via RANDR extension
              NOTE: has no visual effect under Wayland compositors
```

For `setBrightness`, the strategy is selected by the `id` prefix:

| ID prefix | Strategy |
|-----------|----------|
| `backlight:intel_backlight` | logind D-Bus → sysfs write fallback |
| `ddc:1` | `ddcutil setvcp 10 <pct> --display 1` |
| `xrandr:DP-1` | `xrandr --output DP-1 --brightness <val>` |

### 3.4 Display Detection Pipeline

```
brightness_plugin_register()
          │
          ▼
  handle_method_call("getDisplays")
          │
          ├─ get_backlight_displays()
          │     opendir("/sys/class/backlight")
          │     for each entry:
          │       read brightness  → cur_val
          │       read max_brightness → max_val
          │       push { id:"backlight:<name>", brightness: cur/max }
          │
          ├─ get_ddc_displays()
          │     run_cmd("ddcutil detect --brief")
          │     parse "Display N" lines → list of ints
          │     for each N:
          │       run_cmd("ddcutil getvcp 10 --display N --brief")
          │       parse "VCP 10 C <cur> <max>" → brightness ratio
          │       run_cmd("ddcutil query --display N | grep 'Monitor name'")
          │       push { id:"ddc:N", name, brightness }
          │
          └─ get_xrandr_displays()  [only if ddc found nothing AND not Wayland]
                run_cmd("xrandr --verbose")
                parse connected outputs + Brightness: line
                push { id:"xrandr:<output>", brightness }
```

### 3.5 Data Flow: Setting Brightness

```
User drags slider
       │
       ▼
_onBrightnessChanged(display, value)
       │
       ├─ setState() → UI updates immediately (optimistic update)
       │
       └─ debounceTimer (16 ms) fires
               │
               ▼
       BrightnessService.setBrightness(displayId, brightness)
               │
               ▼  MethodChannel call
       handle_method_call("setBrightness")
               │
               ├─ backlight:* → logind D-Bus SetBrightness
               │                  └─ fallback: write to sysfs
               │
               ├─ ddc:N      → ddcutil setvcp 10 <pct> --display N
               │
               └─ xrandr:*   → xrandr --output <name> --brightness <val>
                       │
                       ▼
               returns bool ok
                       │
               if !ok → SnackBar error + reload from hardware
```

The 16 ms debounce prevents flooding the I²C bus when the slider is dragged continuously. DDC/CI commands are slow (~50–200 ms each); without debouncing, every pixel of slider travel would queue a command and the UI would feel laggy.

---

## 4. Code Walkthrough

### 4.1 Dart Layer

#### `lib/main.dart`

Entry point. Creates `BSDisplayControlApp`, a `StatelessWidget` wrapping `MaterialApp`. Two themes are defined — light and dark — both using Material 3 with a deep-blue seed color (`#1565C0`). `ThemeMode.system` means the OS dark-mode preference is respected automatically with no user toggle needed.

#### `lib/models/display_info.dart`

`DisplayInfo` is an immutable data class representing one physical display. Key design points:

- **`final class`** — cannot be subclassed, making it safe to use as a value in a `List` without worrying about subclass equality surprises.
- **Equality by `id`** — two `DisplayInfo` objects are equal if they have the same `id`, regardless of `brightness`. This matters for `setState` comparisons: when the slider updates brightness, Flutter can identify which card to re-render.
- **`copyWith`** — lets the home screen produce an updated list (`_displays.map(d => d.id == target ? d.copyWith(brightness: v) : d)`) without mutating anything. Immutability keeps the UI state predictable.
- **`fromMap` with explicit type checks** — the data comes over a binary channel from C++. A malformed map (e.g., `null` brightness) will throw a `FormatException` with a clear message rather than a Dart null-pointer crash deep in a widget build.

#### `lib/services/brightness_service.dart`

A singleton service class that owns the `MethodChannel`. It is the only place in Dart that knows the channel name string, so a rename only requires changing one file. Methods are `async` because channel calls are asynchronous I/O — the Dart event loop yields until the C++ side responds.

#### `lib/screens/home_screen.dart`

`HomeScreen` is a `StatefulWidget` managing three pieces of state: `_displays`, `_isLoading`, and `_error`. A pattern-match `switch` on `(_isLoading, _error)` selects one of three views:

| State | Widget shown |
|-------|--------------|
| loading | `CircularProgressIndicator` |
| error string present | `_ErrorView` with the error message and a Retry button |
| no displays | `_EmptyView` with setup instructions |
| displays list | `ListView.builder` of `DisplayBrightnessCard` |

The debounce timer is stored on the widget's state and cancelled in `dispose()` to avoid calling `setState` on an unmounted widget — a common Flutter bug.

#### `lib/widgets/display_brightness_card.dart`

`StatelessWidget` — it receives the current `DisplayInfo` and an `onBrightnessChanged` callback, so it has no internal state. All state lives in `HomeScreen`. This is a deliberate single-source-of-truth design: if the platform reports a brightness that differs from what the slider shows (hardware clamp, rounding), a reload will always bring the UI back to the true value.

The four preset buttons highlight when the current brightness is within 1 % of the preset value (`abs(brightness - value) < 0.01`), giving the user instant visual feedback that a preset is active.

### 4.2 Native Plugin (C++)

#### `linux/runner/brightness_plugin.h` / `.cc`

The plugin is compiled into the application binary (not a shared library). It exposes one public function, `brightness_plugin_register`, called from `my_application.cc` during startup.

**`brightness_plugin_register`**

```cpp
void brightness_plugin_register(FlPluginRegistry* registry) {
    // Get a registrar scoped to this plugin's name
    FlPluginRegistrar* registrar = fl_plugin_registry_get_registrar_for_plugin(...);
    // Create a MethodChannel with the standard binary codec
    g_channel = fl_method_channel_new(messenger, "com.bsdisplaycontrol/brightness", ...);
    // Install the dispatch function
    fl_method_channel_set_method_call_handler(g_channel, handle_method_call, ...);
}
```

A single static `FlMethodChannel*` is kept alive for the process lifetime. Flutter's GObject memory model requires this — if the channel is released, incoming calls will silently be dropped.

**`run_cmd`**

```cpp
static std::string run_cmd(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    // read output into string
    pclose(pipe);
    return result;
}
```

A minimal wrapper around `popen`. All stderr is redirected to `/dev/null` (`2>/dev/null`) in the callers so that ddcutil's verbose diagnostic output does not interfere with parsing. `popen` is used instead of `exec` because the plugin needs to capture stdout as a string and parse it.

**`ddcutil_cmd`**

```cpp
static std::string ddcutil_cmd() {
    const char* snap = getenv("SNAP");
    if (snap) {
        std::string path = std::string(snap) + "/usr/bin/ddcutil";
        if (/* file exists */) return path;
    }
    return "ddcutil";
}
```

When running as a Snap, `$SNAP` is set to the snap's root (e.g., `/snap/bsdisplaycontrol/current`). This function resolves the absolute path to the bundled ddcutil so the correct binary is always used regardless of the system's `PATH`. When not in a Snap, it falls back to `"ddcutil"` which is found via the process's inherited `PATH`.

**`get_backlight_displays`**

Reads `/sys/class/backlight/` — a virtual filesystem maintained by the kernel's backlight driver. Each subdirectory corresponds to one panel. Files `brightness` and `max_brightness` are plain integers. The ratio `cur / max` gives a 0–1 float.

**`get_ddc_displays`**

DDC/CI (Display Data Channel / Command Interface) is a standard defined in VESA MCCS that allows a host to send commands to a monitor over the I²C bus embedded in the HDMI/DisplayPort cable. VCP feature code `0x10` is the "Luminance" control.

The plugin shells out to `ddcutil` rather than implementing the I²C protocol directly for two reasons:
1. `ddcutil` handles the wide variety of monitor firmware quirks (timing, retries, protocol variants) that would require thousands of lines of C to replicate.
2. `ddcutil` already manages the `/dev/i2c-*` file descriptor lifecycle safely.

The output format of `ddcutil getvcp 10 --brief` is:
```
VCP 10 C 60 100
         ^  ^
         |  max value (always 100 for brightness)
         current value
```

**`get_xrandr_displays`** (X11 only)

On X11, `xrandr --output <name> --brightness <value>` adjusts the CRTC gamma ramp — a hardware lookup table in the GPU that maps each 8-bit input value to a 16-bit output. Setting brightness to 0.5 scales all output values by 0.5, effectively halving the luminance in software.

**This does not work on Wayland.** Under GNOME/Mutter with XWayland, the RandR protocol is emulated: `xrandr` reports `RANDR Emulation: 1` on all outputs, and gamma change requests are accepted but never forwarded to the compositor's DRM/KMS pipeline. The `is_wayland()` check (`getenv("WAYLAND_DISPLAY") != nullptr`) short-circuits this path entirely to avoid confusing behaviour where the slider appears to work but the screen does not change.

**Error path when no displays found**

Rather than returning an empty list (which shows "0 displays detected" with no explanation), the plugin returns a `PlatformException` with a message that tells the user exactly what to install and which group to join. This surfaces in `_ErrorView` in the UI.

### 4.3 GTK Application Shell

#### `linux/runner/main.cc`

Creates `MyApplication` — a `GtkApplication` subclass — and calls `g_application_run`. This enters the GLib main loop, which drives both GTK events and Flutter's rendering engine.

#### `linux/runner/my_application.cc`

Implements the GObject lifecycle for the application:

- **`my_application_activate`** — creates the GTK window, creates a `FlView` (Flutter's GTK widget), sets the window size to 1280×720, registers all plugins, then connects to the `first-frame` signal so the window is shown only after Flutter has rendered its first frame (avoids a flash of empty black window).
- **Header bar detection** — on X11, the code checks whether the window manager is GNOME Shell by name. If it is (or if running on Wayland), a `GtkHeaderBar` is used for native GNOME integration. On other WMs (KDE, i3, etc.), a traditional title bar is used to avoid the double-border look that `GtkHeaderBar` causes in non-GNOME environments.
- **Plugin registration** — `fl_register_plugins` registers any Flutter plugins listed in `generated_plugin_registrant.cc`. `brightness_plugin_register` registers the custom brightness plugin. Both are called on the same `FlPluginRegistry` (the `FlView`).

### 4.4 Build System (CMake)

Flutter's Linux build uses a two-level CMake structure:

```
linux/CMakeLists.txt          ← top-level: compiler setup, dependencies
    └── runner/CMakeLists.txt ← defines the executable target
    └── flutter/CMakeLists.txt ← managed by Flutter tool (do not edit)
```

**C++ standard**: `cxx_std_14` is required in `APPLY_STANDARD_SETTINGS`. C++17 features like `std::filesystem` and structured bindings were deliberately avoided for maximum compiler compatibility.

**clang++/GCC 13 workaround**: On Ubuntu 25.10, `clang++` auto-selects GCC 15 as its toolchain, but GCC 15 ships without C++ dev headers and `libstdc++.so` in the standard location. The CMakeLists detects this and injects `--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13` so that `<type_traits>` and `libstdc++` are resolved from GCC 13:

```cmake
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_GCC13_DIR "/usr/lib/gcc/x86_64-linux-gnu/13")
  if(EXISTS "${_GCC13_DIR}/libstdc++.so")
    add_compile_options("--gcc-install-dir=${_GCC13_DIR}")
    link_directories(${_GCC13_DIR})
  endif()
endif()
```

**RPATH**: `CMAKE_INSTALL_RPATH "$ORIGIN/lib"` ensures the installed binary finds `libflutter_linux_gtk.so` in the `lib/` subdirectory next to the executable without requiring `LD_LIBRARY_PATH` to be set at runtime.

---

## 5. Design Decisions & Justifications

### Why Flutter for a Linux desktop utility?

Flutter's Linux support produces a native GTK window with hardware-accelerated rendering. For a brightness controller, the UI is simple enough that the framework overhead is negligible. The benefit is that any future UI extension (animations, multi-monitor drag-and-drop layout) is trivial in Flutter but painful in raw GTK C.

### Why a custom C++ plugin instead of a Flutter package?

No existing pub.dev package provides DDC/CI brightness control on Linux. The required operations (sysfs reads, I²C via ddcutil, logind D-Bus) are all Linux-specific system calls or subprocesses — there is no cross-platform abstraction worth making. A thin, purpose-built plugin avoids pulling in a heavy dependency for functionality that is ultimately three file reads and a subprocess call.

### Why shell out to ddcutil instead of using the I²C API directly?

Implementing DDC/CI correctly requires handling:
- Multiple I²C bus scanning strategies
- Monitor EDID parsing to identify which bus corresponds to which display
- MCCS (Monitor Control Command Set) version negotiation
- Retry logic for monitors with slow firmware responses
- Quirk tables for non-compliant monitors (e.g., some Samsung and LG panels)

`ddcutil` handles all of this in ~50k lines of well-maintained C. Writing an equivalent would be the entire project. The subprocess approach has measurable overhead (~100–300 ms per command) which is why the 16 ms debounce is essential — it coalesces rapid slider drags into a single command.

### Why is ddcutil bundled in the Snap instead of declared as a dependency?

`ddcutil` is not available in Ubuntu's `main` repository (it is in `universe`). Declaring it as an external dependency would require the user to enable the universe repository and run `apt install ddcutil` manually. Bundling it means the Snap is genuinely self-contained: install and run.

### Why classic Snap confinement instead of strict?

Strict confinement uses AppArmor profiles to whitelist specific system accesses. The interface that would grant `/dev/i2c-*` access (`raw-i2c`) does not exist in snapd 2.75.x. Without it, AppArmor blocks every `open()` call on i2c devices with `EPERM`, even if the user is in the `i2c` group. Classic confinement runs the snap with the same privileges as a regular binary — the user's group memberships apply normally. Since this application is a developer-installed sideload anyway (installed with `--dangerous`), classic confinement is the pragmatic and correct choice.

### Why is xrandr skipped on Wayland?

On GNOME Wayland, XWayland presents a `RANDR Emulation: 1` layer to X11 clients. The layer accepts gamma ramp commands but does not forward them to Mutter's KMS/DRM pipeline. The screen does not change. Silently accepting the `xrandr` command (exit code 0) while doing nothing would make the app appear to work while actually doing nothing. Better to detect Wayland and surface a "ddcutil required" error immediately.

### Why debounce at 16 ms (one frame) instead of a larger value?

16 ms is one frame at 60 Hz. The optimistic UI update happens immediately in `setState`, so the slider visually tracks the finger at 60 fps. The actual hardware command fires at most once per rendered frame, which is fast enough to feel responsive while staying well within the DDC/CI command rate that monitors can handle. A larger debounce (e.g., 200 ms) would make the percentage badge lag visibly behind the slider.

---

## 6. Prerequisites

### 6.1 Development Dependencies

| Tool | Version | Purpose |
|------|---------|---------|
| Flutter | ≥ 3.32 | Dart SDK + build tooling |
| Dart | ≥ 3.10 (via Flutter) | Language runtime |
| CMake | ≥ 3.13 | C++ build system |
| Ninja | any | CMake generator used by Flutter |
| clang++ | any recent | C++ compiler (g++ also works) |
| libgtk-3-dev | any | GTK headers for the C++ plugin |
| libstdc++-dev (GCC 13) | 13.x | C++ standard library headers |
| pkg-config | any | Locates GTK at compile time |

Install on Ubuntu / Debian:

```bash
sudo apt install cmake ninja-build clang libgtk-3-dev pkg-config libstdc++-13-dev
```

### 6.2 Runtime Dependencies

#### For the direct binary (non-Snap):

| Package | Why |
|---------|-----|
| `ddcutil` | DDC/CI brightness control for external monitors |
| `libi2c0` | I²C userspace library used by ddcutil |

Install:
```bash
sudo apt install ddcutil libi2c0
```

You must also be in the `i2c` group (see [Section 9](#9-permissions--i2c-group-setup)).

#### For the Snap:

Nothing. The Snap bundles ddcutil and libi2c0 internally and sets `LD_LIBRARY_PATH` automatically.

---

## 7. Building the Project

### 7.1 Development Build (flutter run)

```bash
# 1. Clone the repository
git clone <repo-url>
cd BSDisplayControl

# 2. Fetch Dart dependencies
flutter pub get

# 3. Run in debug mode (hot-reload enabled)
flutter run -d linux
```

Flutter will invoke CMake and Ninja automatically to compile the C++ plugin. The first build takes ~2 minutes; subsequent builds with hot-reload are instant.

**Enable Flutter Linux desktop support if not already on:**

```bash
flutter config --enable-linux-desktop
```

### 7.2 Release Build

```bash
flutter build linux --release
```

Output is placed at:
```
build/linux/x64/release/bundle/
├── bs_display_control          ← main executable
├── lib/
│   └── libflutter_linux_gtk.so ← Flutter engine
└── data/
    ├── flutter_assets/         ← Dart assets, fonts, ICU data
    └── app.so                  ← AOT-compiled Dart code
```

The entire `bundle/` directory is self-contained and relocatable — copy it anywhere and the binary runs as long as the system has GTK 3 and ddcutil installed.

---

## 8. Deployment

### 8.1 Running the Release Bundle Directly

```bash
# Prerequisites: ddcutil installed, user in i2c group
sudo apt install ddcutil

# Run
./build/linux/x64/release/bundle/bs_display_control
```

To install system-wide:

```bash
# Copy the bundle to a permanent location
sudo cp -r build/linux/x64/release/bundle /opt/bsdisplaycontrol

# Create a launcher script
sudo tee /usr/local/bin/bsdisplaycontrol > /dev/null << 'EOF'
#!/bin/bash
exec /opt/bsdisplaycontrol/bs_display_control "$@"
EOF
sudo chmod +x /usr/local/bin/bsdisplaycontrol
```

### 8.2 Building and Installing the Snap Package

The Snap is the recommended distribution method. It bundles ddcutil, handles library paths, and installs with a single command.

#### Step 1 — Install Snapcraft

```bash
sudo snap install snapcraft --classic
```

#### Step 2 — Build the Snap

```bash
cd BSDisplayControl
snapcraft
```

Snapcraft builds inside a container (LXD or Multipass). On first run it will prompt to install the container backend. The build takes ~5–10 minutes. Output:

```
bsdisplaycontrol_0.0.3_amd64.snap
```

#### Step 3 — Install the Snap

```bash
sudo snap install --dangerous --classic bsdisplaycontrol_0.0.3_amd64.snap
```

- `--dangerous` is required for locally-built snaps not signed by the Snap Store.
- `--classic` is required because the snap uses classic confinement to access `/dev/i2c-*`.

#### Step 4 — Launch

```bash
snap run bsdisplaycontrol
# or simply
bsdisplaycontrol
```

#### Updating the Snap After Code Changes

```bash
# Bump the version in snap/snapcraft.yaml if desired, then:
snapcraft
sudo snap install --dangerous --classic bsdisplaycontrol_<version>_amd64.snap
```

Snapcraft is incremental — it reuses cached stages and only rebuilds what changed.

#### Snap Internals

```
squashfs-root/
├── bs_display_control          ← Flutter app binary
├── usr/
│   ├── bin/
│   │   ├── ddcutil             ← bundled DDC/CI tool
│   │   └── xrandr              ← bundled X11 brightness fallback
│   └── lib/x86_64-linux-gnu/
│       ├── libi2c.so.0         ← I²C userspace library
│       └── libddcutil.so.*     ← ddcutil shared library
├── lib/
│   └── libflutter_linux_gtk.so ← Flutter engine
└── meta/
    └── snap.yaml               ← processed snap manifest
```

The `snapcraft.yaml` sets:

```yaml
environment:
  PATH: $SNAP/usr/bin:$SNAP/bin:$PATH
  LD_LIBRARY_PATH: $SNAP/usr/lib/x86_64-linux-gnu:$SNAP/usr/lib:$LD_LIBRARY_PATH
```

This ensures that when the app subprocess-execs `ddcutil`, it finds the bundled binary (not a missing/wrong system one) and the binary finds `libi2c.so.0` at `$SNAP/usr/lib/x86_64-linux-gnu/libi2c.so.0`.

---

## 9. Permissions & i2c Group Setup

DDC/CI communicates over the I²C bus exposed as `/dev/i2c-N` character devices. By default these are owned by `root:i2c` with permissions `crw-rw----`, meaning only root and members of the `i2c` group can open them.

**Add your user to the i2c group:**

```bash
sudo usermod -aG i2c $USER
```

**Log out and back in** (or reboot) for the group change to take effect in your session. Verify:

```bash
groups
# should include: ... i2c ...
```

**Verify ddcutil can see your monitors:**

```bash
ddcutil detect
```

Expected output:
```
Display 1
   I2C bus:  /dev/i2c-6
   ...
   Model:    PHL 271E1
```

If you see `Open failed for /dev/i2c-N: errno=EACCES`, the group membership is not active in the current session. Log out and back in.

If you see `Module i2c-dev is not loaded`, load it:

```bash
sudo modprobe i2c-dev
# To persist across reboots:
echo i2c-dev | sudo tee /etc/modules-load.d/i2c-dev.conf
```

---

## 10. Troubleshooting

### "No displays found" error in the app

| Symptom | Cause | Fix |
|---------|-------|-----|
| Running on Wayland, external monitors only | ddcutil not installed | `sudo apt install ddcutil && sudo usermod -aG i2c $USER`, then log out/in |
| `Open failed: EPERM` in ddcutil | User not in i2c group | See [Section 9](#9-permissions--i2c-group-setup) |
| `Module i2c-dev is not loaded` | Kernel module missing | `sudo modprobe i2c-dev` |
| Monitor not detected by ddcutil | DDC/CI disabled on monitor | Enable DDC/CI in monitor OSD settings |

### Brightness slider moves but nothing changes

| Symptom | Cause | Fix |
|---------|-------|-----|
| Running via Snap, slider returns `false` | libi2c.so.0 not in LD_LIBRARY_PATH | Reinstall latest snap (0.0.3+) |
| Running on Wayland via flutter run, no ddcutil | xrandr fallback silently no-ops | Install ddcutil (see above) |
| DDC command succeeds but monitor ignores it | Some monitors require DDC enabled in OSD | Check monitor settings for "DDC/CI" option |

### Build fails: `cannot find -lstdc++`

clang++ selected a GCC installation that has no dev headers. The fix is already in `linux/CMakeLists.txt` (the `--gcc-install-dir` block), but it assumes GCC 13 dev files are installed:

```bash
sudo apt install libstdc++-13-dev
```

### Build fails: `'type_traits' file not found`

Same root cause as above. The CMakeLists fix requires a fresh CMake cache:

```bash
rm build/linux/x64/release/CMakeCache.txt
flutter build linux --release
```

### Snap build fails: `Extension 'gnome' does not support confinement 'classic'`

The gnome extension is incompatible with classic confinement. It was removed from `snap/snapcraft.yaml` in version 0.0.3. If you see this error, make sure you are using the latest `snapcraft.yaml`.

---

## 11. Project Structure

```
BSDisplayControl/
│
├── lib/                              Dart source
│   ├── main.dart                     App entry point, theme setup
│   ├── models/
│   │   └── display_info.dart         Immutable display data model
│   ├── screens/
│   │   └── home_screen.dart          Main screen, state management, debounce
│   ├── services/
│   │   └── brightness_service.dart   MethodChannel client (singleton)
│   └── widgets/
│       └── display_brightness_card.dart  Per-display card with slider + presets
│
├── linux/                            Linux-specific native code
│   ├── CMakeLists.txt                Top-level CMake: compiler quirks, GTK deps
│   ├── runner/
│   │   ├── main.cc                   GApplication entry point
│   │   ├── my_application.cc/.h      GtkApplication subclass, window setup
│   │   ├── brightness_plugin.cc      Native brightness plugin (all strategies)
│   │   └── brightness_plugin.h       Plugin registration declaration
│   └── flutter/
│       ├── CMakeLists.txt            Flutter tool CMake (do not edit)
│       └── generated_plugin_registrant.cc  Auto-generated plugin registry
│
├── snap/
│   └── snapcraft.yaml                Snap package definition (classic, v0.0.3)
│
├── pubspec.yaml                      Dart/Flutter dependencies
├── analysis_options.yaml             Dart linter configuration
└── README.md                         This file
```

---

## License

See [LICENSE](LICENSE).

# BSDisplayControl - Project Overview

## What Is This Project?

BSDisplayControl is a **cross-platform desktop application** that lets you control the hardware brightness of every monitor connected to your computer -- built-in laptop screens and external monitors alike -- from a single unified UI.

Think of it like the brightness slider on your phone, but for all your desktop monitors at once.

### The Problem It Solves

- On Windows, you can adjust your laptop screen brightness from the system tray, but external monitors require opening their OSD (on-screen display) menus and navigating physical buttons.
- On macOS, brightness control works for the built-in display, but external monitors are hit-or-miss.
- On Linux, there's no standard GUI for monitor brightness at all.

BSDisplayControl provides one app that handles all monitors on all three desktop operating systems.

## Technology Stack

| Layer             | Technology              | Language     |
| ----------------- | ----------------------- | ------------ |
| UI Framework      | Flutter (desktop)       | Dart         |
| Windows Native    | Win32 API + WMI         | C++17        |
| macOS Native      | Cocoa + IOKit + Private Frameworks | Swift        |
| Linux Native      | GTK3 + DRM/sysfs + I2C | C++17        |
| Build System      | Flutter CLI + CMake (Win/Linux), Xcode (macOS) | -  |

**No third-party packages** are used beyond Flutter itself and `flutter_lints`. All brightness control is implemented directly against OS-level APIs.

## High-Level Architecture

The app follows Flutter's **Platform Channel** pattern. If you're familiar with ASP.NET Core, think of it like a frontend SPA (the Dart/Flutter UI) calling a backend API (the native platform code) -- except instead of HTTP, they communicate over an in-process binary message channel.

```
+------------------------------------------------------------------+
|                     Flutter / Dart Layer                          |
|                                                                  |
|  main.dart --> HomeScreen --> DisplayBrightnessCard               |
|                     |                                            |
|              BrightnessService                                   |
|              (MethodChannel client)                              |
+------------------------------------------------------------------+
                        |
                  MethodChannel
              "com.chandanbsd.bsdisplaycontrol/brightness"
                        |
          +-------------+-------------+
          |             |             |
  +-------v------+ +---v--------+ +-v-----------+
  |   Windows    | |   macOS    | |    Linux     |
  | flutter_     | | MainFlutter| | my_          |
  | window.cpp   | | Window     | | application  |
  | (C++, 431    | | .swift     | | .cc          |
  |  lines)      | | (338 lines)| | (865 lines)  |
  +--------------+ +------------+ +--------------+
        |                |               |
  DDC/CI via       DisplayServices   DDC/CI via
  Win32 Dxva2      / CoreDisplay /   raw I2C
  + WMI fallback   IOKit fallback    + ddcutil
                                     + xrandr
                                       fallback
```

### Analogies for Web Developers

| Flutter Concept | Web Equivalent |
| --- | --- |
| `MaterialApp` (in `main.dart`) | Your Angular `AppModule` with routing and theme |
| `HomeScreen` (StatefulWidget) | A smart Angular component with `ngOnInit` and state |
| `DisplayBrightnessCard` (StatelessWidget) | A presentational/dumb Angular component |
| `BrightnessService` (singleton) | An Angular injectable service (`@Injectable({providedIn: 'root'})`) |
| `MethodChannel` | Like an HttpClient, but calls native code instead of a REST API |
| `DisplayInfo` model | A TypeScript interface / C# record DTO |
| Platform code (C++/Swift) | The ASP.NET Core backend controllers and services |

## Project Directory Structure

```
BSDisplayControl/
|
|-- lib/                          # Dart/Flutter source code (the "frontend")
|   |-- main.dart                 # App entry point, MaterialApp config
|   |-- models/
|   |   +-- display_info.dart     # DisplayInfo data model (like a DTO)
|   |-- services/
|   |   +-- brightness_service.dart  # Platform channel client (like an API service)
|   |-- screens/
|   |   +-- home_screen.dart      # Main screen with display list (smart component)
|   +-- widgets/
|       +-- display_brightness_card.dart  # Per-display card (dumb component)
|
|-- windows/                      # Windows platform project
|   |-- CMakeLists.txt            # Top-level Windows CMake build config
|   +-- runner/
|       |-- CMakeLists.txt        # Runner build config (links Dxva2, WMI libs)
|       |-- main.cpp              # Win32 entry point (wWinMain)
|       |-- flutter_window.h/.cpp # Brightness channel handler + DDC/CI + WMI
|       |-- win32_window.h/.cpp   # Base Win32 window class (DPI-aware)
|       |-- utils.h/.cpp          # Console attach, UTF conversion helpers
|       |-- resource.h            # Icon resource ID
|       |-- Runner.rc             # Windows resource script (icon, version info)
|       +-- runner.exe.manifest   # DPI awareness manifest
|
|-- macos/                        # macOS platform project
|   |-- Runner.xcodeproj/         # Xcode project
|   |-- Runner.xcworkspace/       # Xcode workspace
|   +-- Runner/
|       |-- AppDelegate.swift     # NSApp delegate (terminate-on-close)
|       |-- MainFlutterWindow.swift  # Brightness channel + 3-tier brightness API
|       |-- Info.plist            # Bundle config
|       |-- *.entitlements        # App Sandbox disabled (needed for IOKit)
|       +-- Configs/              # Xcode build configs (Debug/Release)
|
|-- linux/                        # Linux platform project
|   |-- CMakeLists.txt            # Top-level Linux CMake (GCC toolchain fix)
|   +-- runner/
|       |-- CMakeLists.txt        # Runner build config
|       |-- main.cc               # GTK entry point
|       |-- my_application.h/.cc  # Brightness channel + DRM + DDC/CI + I2C
|
|-- test/
|   +-- widget_test.dart          # Basic smoke test
|
|-- pubspec.yaml                  # Flutter project manifest
|-- analysis_options.yaml         # Dart lint rules
+-- docs/                         # This documentation
```

## Data Flow: What Happens When You Move a Slider

Here's the complete request lifecycle when a user drags a brightness slider:

```
1. User drags slider on DisplayBrightnessCard widget
                    |
2. onBrightnessChanged callback fires (display_brightness_card.dart:99)
                    |
3. HomeScreen._onBrightnessChanged is called (home_screen.dart:59)
   |
   |-- a) setState() updates UI immediately (optimistic update)
   |
   +-- b) 16ms debounce timer starts
                    |
4. Timer fires --> BrightnessService.setBrightness() (brightness_service.dart:28)
                    |
5. MethodChannel.invokeMethod('setBrightness', {displayId, brightness})
   Serializes args to binary, sends to native side
                    |
6. Platform-specific handler receives the call:
   |
   |-- [Windows] SetMonitorBrightnessById() --> DDC/CI or WMI
   |-- [macOS]   setBrightness()            --> DisplayServices or CoreDisplay or IOKit
   +-- [Linux]   SetDisplayBrightness()     --> I2C DDC/CI or ddcutil or xrandr
                    |
7. Monitor hardware processes the DDC/CI command
   (actual backlight changes)
                    |
8. Native code returns bool (success/failure)
                    |
9. MethodChannel sends result back to Dart
                    |
10. If failed: SnackBar error + reload actual brightness from hardware
    If succeeded: done (UI already updated in step 3a)
```

## Key Design Decisions

1. **No third-party dependencies** -- The app only depends on Flutter SDK and the platform's native APIs. This avoids version conflicts and keeps the binary small.

2. **Optimistic UI updates** -- The slider updates instantly in the UI before the native call completes. This makes the app feel responsive even though DDC/CI commands can take 50-100ms to round-trip.

3. **16ms debounce** -- Slider drag events fire continuously. Without debouncing, the app would flood the I2C bus with DDC/CI commands. 16ms (~60fps) throttles this to a manageable rate.

4. **Fallback chains** -- Each platform tries multiple APIs in order. If the best API fails (e.g., DisplayServices on macOS), it falls back to less reliable but more compatible alternatives (CoreDisplay, IOKit). Linux has the deepest chain: raw I2C -> ddcutil CLI -> xrandr gamma.

5. **Hardware brightness, not software gamma** -- The app controls actual monitor backlight levels via DDC/CI, not the software gamma curve (which washes out colors). The xrandr fallback on Linux is the exception -- it's software-only and used as a last resort.

## Version and SDK Requirements

| Requirement | Version |
| --- | --- |
| Dart SDK | >= 3.10.7 |
| Flutter | Matching Dart SDK |
| Windows | 10+ (with DDC/CI-capable monitors) |
| macOS | 10.14+ (Mojave or later) |
| Linux | Any distro with GTK 3, DRM, I2C support |

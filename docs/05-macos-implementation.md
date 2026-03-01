# macOS Native Implementation

**Primary file:** `macos/Runner/MainFlutterWindow.swift` (338 lines)
**App delegate:** `macos/Runner/AppDelegate.swift`
**Language:** Swift

This document covers how BSDisplayControl reads and writes monitor brightness on macOS.

## Architecture Overview

```
MainFlutterWindow.awakeFromNib()
         |
    FlutterMethodChannel("com.chandanbsd.bsdisplaycontrol/brightness")
         |
    BrightnessMethodHandler
         |
         +-- "getDisplays"  --> getDisplays()
         |                       |
         |                       +-- getDisplayBrightness(displayID)
         |                       |    |
         |                       |    +-- 1. DisplayServices (private framework)
         |                       |    +-- 2. CoreDisplay (private framework)
         |                       |    +-- 3. IOKit (IODisplayConnect)
         |                       |
         |                       +-- displayName(for: displayID)
         |                            |
         |                            +-- CGDisplayCopyDisplayMode (resolution)
         |                            +-- IOKit (EDID product name)
         |
         +-- "setBrightness" --> setBrightness(displayId:brightness:)
                                  |
                                  +-- 1. DisplayServices
                                  +-- 2. CoreDisplay
                                  +-- 3. IOKit
```

## The Three-Tier Brightness API Strategy

macOS does not provide a single public API for controlling all displays' brightness. The app uses three APIs in fallback order. This is a pragmatic approach because Apple's public APIs are limited and the private frameworks, while undocumented, are widely used by third-party apps.

### Tier 1: DisplayServices (Private Framework)

```swift
private let displayServicesHandle: UnsafeMutableRawPointer? = {
  dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_LAZY)
}()
```

- **What it is:** An Apple private framework that provides the same brightness control used by the macOS Control Center slider.
- **Loaded via:** `dlopen()` at runtime (not linked at compile time, since it's private).
- **Functions used:** `DisplayServicesGetBrightness(displayID, &brightness)` and `DisplayServicesSetBrightness(displayID, brightness)`.
- **Works for:** Built-in displays (MacBook panels). Reliable on macOS 10.14+.
- **Returns:** `Int32` result code; `0` means success.

The function pointers are resolved via `dlsym()` and stored as typed closures:

```swift
typealias DSGetBrightnessFunc = @convention(c) (UInt32, UnsafeMutablePointer<Float>) -> Int32
typealias DSSetBrightnessFunc = @convention(c) (UInt32, Float) -> Int32
```

### Tier 2: CoreDisplay (Private Framework)

```swift
private let coreDisplayHandle: UnsafeMutableRawPointer? = {
  dlopen("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay", RTLD_LAZY)
}()
```

- **What it is:** A lower-level Apple framework used by the display subsystem.
- **Functions used:** `CoreDisplay_Display_GetUserBrightness(displayID)` and `CoreDisplay_Display_SetUserBrightness(displayID, brightness)`.
- **Works for:** Some external monitors (especially Thunderbolt/USB-C displays).
- **Note:** The set function returns `Void` -- there's no error reporting. The get function returns a `Double` in the 0.0-1.0 range; values outside this range indicate failure.

### Tier 3: IOKit (Public Framework)

```swift
IODisplayGetFloatParameter(service, 0, kIODisplayBrightnessKey as CFString, &brightness)
IODisplaySetFloatParameter(service, 0, kIODisplayBrightnessKey as CFString, brightness)
```

- **What it is:** Apple's public I/O Kit framework for interacting with hardware drivers.
- **Works for:** Displays whose drivers expose the `IODisplayBrightnessKey` parameter.
- **Matching:** The code must find the correct `IODisplayConnect` service by matching vendor and product IDs from CoreGraphics to IOKit's registry.

## Display Enumeration

### Getting the Display List

```swift
CGGetOnlineDisplayList(0, nil, &displayCount)        // Get count
CGGetOnlineDisplayList(displayCount, &onlineDisplays, &displayCount)  // Get IDs
```

`CGGetOnlineDisplayList` returns `CGDirectDisplayID` values for all currently active displays (including those that are mirrored or in hardware sleep).

### Display Name Resolution

The `displayName(for:)` method resolves human-readable names:

1. **Built-in displays:** Returns `"Built-in Display (WxH)"` using `CGDisplayCopyDisplayMode()`.

2. **External displays:** Searches IOKit's `IODisplayConnect` services:
   - Gets vendor/product IDs from CoreGraphics: `CGDisplayVendorNumber()` / `CGDisplayModelNumber()`
   - Iterates `IODisplayConnect` services matching those IDs
   - Reads `DisplayProductName` from `IODisplayCreateInfoDictionary()`
   - Returns `"ModelName (WxH)"` (e.g., `"DELL U2412M (1920x1200)"`)

3. **Fallback:** `"Display <id> (WxH)"`

### IOKit Service Matching

The IOKit brightness functions require a service handle for the specific display. The code matches by vendor and product ID:

```swift
let cgVendor = CGDisplayVendorNumber(displayID)    // e.g., 0x10AC (Dell)
let cgModel  = CGDisplayModelNumber(displayID)     // e.g., 0xD06E

var iterator: io_iterator_t = 0
IOServiceGetMatchingServices(port, IOServiceMatching("IODisplayConnect"), &iterator)

// Iterate all IODisplayConnect services, match by vendor+product
while service != 0 {
  let info = IODisplayCreateInfoDictionary(service, ...)
  if info["DisplayVendorID"] == cgVendor && info["DisplayProductID"] == cgModel {
    // Found it -- use this service handle
  }
}
```

### macOS Version Compatibility

The code handles the `kIOMainPortDefault` rename:

```swift
let port: mach_port_t
if #available(macOS 12.0, *) {
  port = kIOMainPortDefault      // macOS 12+
} else {
  port = 0                        // Earlier macOS (kIOMasterPortDefault was 0)
}
```

## Built-in vs External Detection

```swift
let isBuiltIn = CGDisplayIsBuiltin(displayID) != 0
```

`CGDisplayIsBuiltin()` is a CoreGraphics function that returns `1` for the built-in laptop panel and `0` for all external monitors. This is more reliable than the heuristic used on Windows.

## App Delegate

**`AppDelegate.swift`** is minimal:

```swift
@main
class AppDelegate: FlutterAppDelegate {
  override func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    return true    // Quit app when window is closed (not just hidden)
  }

  override func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool {
    return true    // Modern state restoration
  }
}
```

## Entitlements

Both Debug and Release entitlements disable the **App Sandbox** (`com.apple.security.app-sandbox = false`). This is required because:

1. IOKit service access is blocked by the sandbox
2. `dlopen()` of private frameworks fails in a sandboxed app

**Implication:** The app cannot be distributed via the Mac App Store (which requires sandboxing). It would need to be distributed directly or via Homebrew/other channels.

## Build Configuration

### Xcode Project Structure

```
Runner.xcworkspace/
+-- Runner.xcodeproj/
    +-- Runner (target)
    |   +-- AppDelegate.swift
    |   +-- MainFlutterWindow.swift
    |   +-- Info.plist
    |   +-- Assets.xcassets (app icons)
    +-- RunnerTests (target)
        +-- RunnerTests.swift (placeholder)
```

### Key Build Settings (from Configs/)

| Setting | Value |
| --- | --- |
| `PRODUCT_NAME` | `bs_display_control` |
| `PRODUCT_BUNDLE_IDENTIFIER` | `com.chandanbsd.bsdisplaycontrol.bsDisplayControl` |
| Warnings | `-Wall`, strict nullable, shadow, unreachable code warnings |
| Minimum deployment target | Set in Xcode project (typically macOS 10.14+) |

### Linked Frameworks

The following frameworks are imported in code (linked automatically by Swift):

| Framework | Purpose |
| --- | --- |
| `Cocoa` | AppKit, windowing |
| `FlutterMacOS` | Flutter engine and method channel APIs |
| `CoreGraphics` | `CGGetOnlineDisplayList`, `CGDisplayIsBuiltin`, display IDs |
| `IOKit` | `IOServiceGetMatchingServices`, `IODisplayGetFloatParameter` |
| `IOKit.graphics` | Display-specific IOKit constants |

The private frameworks (`DisplayServices`, `CoreDisplay`) are loaded at runtime via `dlopen()` and are NOT linked at build time.

## Limitations

1. **Private framework dependency** -- `DisplayServices` and `CoreDisplay` are undocumented. Apple could remove or change them in any macOS update, though they've been stable for many years.

2. **No App Store distribution** -- Sandbox must be disabled for IOKit and private framework access.

3. **External monitor support varies** -- Not all external monitors expose brightness via IOKit or CoreDisplay. Monitors connected via DDC/CI-capable connections (DisplayPort, HDMI) generally work better.

4. **No DDC/CI on macOS** -- Unlike Linux, this implementation does not directly speak DDC/CI. It relies on Apple's frameworks to do so. For monitors not supported by Apple's stack, brightness control won't work.

5. **Display name matching by vendor/product** -- If two identical monitors are connected (same vendor and product ID), the IOKit matching may return the wrong service. This is an edge case that would require serial number matching to resolve.

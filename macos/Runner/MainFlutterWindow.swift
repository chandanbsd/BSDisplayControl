import Cocoa
import FlutterMacOS
import CoreGraphics
import IOKit
import IOKit.graphics

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    // Register the brightness method channel.
    let channel = FlutterMethodChannel(
      name: "com.bsdisplaycontrol/brightness",
      binaryMessenger: flutterViewController.engine.binaryMessenger
    )
    let brightnessHandler = BrightnessMethodHandler()
    channel.setMethodCallHandler(brightnessHandler.handle)

    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}

// MARK: - DisplayServices private framework bridge
// DisplayServices is a private Apple framework that provides reliable
// brightness get/set for built-in displays on all modern macOS versions.

private let displayServicesHandle: UnsafeMutableRawPointer? = {
  dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_LAZY)
}()

private typealias DSGetBrightnessFunc = @convention(c) (UInt32, UnsafeMutablePointer<Float>) -> Int32
private typealias DSSetBrightnessFunc = @convention(c) (UInt32, Float) -> Int32

private let dsGetBrightness: DSGetBrightnessFunc? = {
  guard let handle = displayServicesHandle,
        let sym = dlsym(handle, "DisplayServicesGetBrightness") else { return nil }
  return unsafeBitCast(sym, to: DSGetBrightnessFunc.self)
}()

private let dsSetBrightness: DSSetBrightnessFunc? = {
  guard let handle = displayServicesHandle,
        let sym = dlsym(handle, "DisplayServicesSetBrightness") else { return nil }
  return unsafeBitCast(sym, to: DSSetBrightnessFunc.self)
}()

// MARK: - CoreDisplay private framework bridge (fallback for external monitors)

private let coreDisplayHandle: UnsafeMutableRawPointer? = {
  dlopen("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay", RTLD_LAZY)
}()

private typealias CDSetBrightnessFunc = @convention(c) (UInt32, Double) -> Void
private typealias CDGetBrightnessFunc = @convention(c) (UInt32) -> Double

private let cdSetBrightness: CDSetBrightnessFunc? = {
  guard let handle = coreDisplayHandle,
        let sym = dlsym(handle, "CoreDisplay_Display_SetUserBrightness") else { return nil }
  return unsafeBitCast(sym, to: CDSetBrightnessFunc.self)
}()

private let cdGetBrightness: CDGetBrightnessFunc? = {
  guard let handle = coreDisplayHandle,
        let sym = dlsym(handle, "CoreDisplay_Display_GetUserBrightness") else { return nil }
  return unsafeBitCast(sym, to: CDGetBrightnessFunc.self)
}()

// MARK: - Brightness Method Channel Handler

final class BrightnessMethodHandler {

  func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "getDisplays":
      result(getDisplays())
    case "setBrightness":
      guard let args = call.arguments as? [String: Any],
            let displayId = args["displayId"] as? String,
            let brightness = args["brightness"] as? Double else {
        result(FlutterError(code: "INVALID_ARGS",
                            message: "Missing displayId or brightness",
                            details: nil))
        return
      }
      let success = setBrightness(displayId: displayId, brightness: Float(brightness))
      result(success)
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  // MARK: - Display Enumeration

  private func getDisplays() -> [[String: Any]] {
    var displays: [[String: Any]] = []

    var displayCount: UInt32 = 0
    CGGetOnlineDisplayList(0, nil, &displayCount)
    guard displayCount > 0 else { return displays }

    var onlineDisplays = [CGDirectDisplayID](repeating: 0, count: Int(displayCount))
    CGGetOnlineDisplayList(displayCount, &onlineDisplays, &displayCount)

    for displayID in onlineDisplays {
      let brightness = getDisplayBrightness(displayID: displayID)
      let isBuiltIn = CGDisplayIsBuiltin(displayID) != 0
      let name = displayName(for: displayID)

      displays.append([
        "id": String(displayID),
        "name": name,
        "brightness": Double(brightness),
        "isBuiltIn": isBuiltIn,
      ])
    }

    return displays
  }

  // MARK: - Get Brightness

  private func getDisplayBrightness(displayID: CGDirectDisplayID) -> Float {
    // 1. Try DisplayServices (most reliable for built-in).
    if let getBrightness = dsGetBrightness {
      var brightness: Float = 0
      let ret = getBrightness(displayID, &brightness)
      if ret == 0 {
        NSLog("[BSDisplayControl] DisplayServices getBrightness for %d = %.2f", displayID, brightness)
        return brightness
      }
    }

    // 2. Try CoreDisplay (works for some external monitors).
    if let getBrightness = cdGetBrightness {
      let brightness = getBrightness(displayID)
      if brightness >= 0.0 && brightness <= 1.0 {
        NSLog("[BSDisplayControl] CoreDisplay getBrightness for %d = %.2f", displayID, brightness)
        return Float(brightness)
      }
    }

    // 3. Fallback: IOKit IODisplayConnect - iterate and match by vendor/product IDs.
    let iokitBrightness = getIOKitBrightness(displayID: displayID)
    if let b = iokitBrightness {
      NSLog("[BSDisplayControl] IOKit getBrightness for %d = %.2f", displayID, b)
      return b
    }

    NSLog("[BSDisplayControl] Could not read brightness for display %d, defaulting to 1.0", displayID)
    return 1.0
  }

  private func getIOKitBrightness(displayID: CGDirectDisplayID) -> Float? {
    let port: mach_port_t
    if #available(macOS 12.0, *) {
      port = kIOMainPortDefault
    } else {
      port = 0
    }

    // Get vendor and product from CoreGraphics to match IOKit service.
    let cgVendor = CGDisplayVendorNumber(displayID)
    let cgModel = CGDisplayModelNumber(displayID)

    var iterator: io_iterator_t = 0
    let kr = IOServiceGetMatchingServices(
      port,
      IOServiceMatching("IODisplayConnect"),
      &iterator
    )
    guard kr == kIOReturnSuccess else { return nil }
    defer { IOObjectRelease(iterator) }

    var service: io_object_t = IOIteratorNext(iterator)
    while service != 0 {
      defer {
        IOObjectRelease(service)
        service = IOIteratorNext(iterator)
      }

      // Match by vendor/product to find the right service.
      if let info = IODisplayCreateInfoDictionary(
        service, IOOptionBits(kIODisplayOnlyPreferredName)
      )?.takeRetainedValue() as? [String: Any] {
        let vendor = info["DisplayVendorID"] as? UInt32 ?? 0
        let product = info["DisplayProductID"] as? UInt32 ?? 0
        if vendor == cgVendor && product == cgModel {
          var brightness: Float = 0
          let paramResult = IODisplayGetFloatParameter(
            service, 0,
            kIODisplayBrightnessKey as CFString,
            &brightness
          )
          if paramResult == kIOReturnSuccess {
            return brightness
          }
        }
      }
    }
    return nil
  }

  // MARK: - Set Brightness

  private func setBrightness(displayId: String, brightness: Float) -> Bool {
    guard let displayIDValue = UInt32(displayId) else { return false }
    let clamped = min(max(brightness, 0.0), 1.0)

    NSLog("[BSDisplayControl] setBrightness for display %d to %.2f", displayIDValue, clamped)

    // 1. Try DisplayServices (most reliable for built-in).
    if let setBrightness = dsSetBrightness {
      let ret = setBrightness(displayIDValue, clamped)
      if ret == 0 {
        NSLog("[BSDisplayControl] DisplayServices setBrightness succeeded")
        return true
      }
    }

    // 2. Try CoreDisplay.
    if let setBrightness = cdSetBrightness {
      setBrightness(displayIDValue, Double(clamped))
      NSLog("[BSDisplayControl] CoreDisplay setBrightness called")
      return true
    }

    // 3. Fallback: IOKit.
    return setIOKitBrightness(displayID: displayIDValue, brightness: clamped)
  }

  private func setIOKitBrightness(displayID: CGDirectDisplayID, brightness: Float) -> Bool {
    let port: mach_port_t
    if #available(macOS 12.0, *) {
      port = kIOMainPortDefault
    } else {
      port = 0
    }

    let cgVendor = CGDisplayVendorNumber(displayID)
    let cgModel = CGDisplayModelNumber(displayID)

    var iterator: io_iterator_t = 0
    let kr = IOServiceGetMatchingServices(
      port,
      IOServiceMatching("IODisplayConnect"),
      &iterator
    )
    guard kr == kIOReturnSuccess else { return false }
    defer { IOObjectRelease(iterator) }

    var service: io_object_t = IOIteratorNext(iterator)
    while service != 0 {
      defer {
        IOObjectRelease(service)
        service = IOIteratorNext(iterator)
      }

      if let info = IODisplayCreateInfoDictionary(
        service, IOOptionBits(kIODisplayOnlyPreferredName)
      )?.takeRetainedValue() as? [String: Any] {
        let vendor = info["DisplayVendorID"] as? UInt32 ?? 0
        let product = info["DisplayProductID"] as? UInt32 ?? 0
        if vendor == cgVendor && product == cgModel {
          let paramResult = IODisplaySetFloatParameter(
            service, 0,
            kIODisplayBrightnessKey as CFString,
            brightness
          )
          if paramResult == kIOReturnSuccess {
            NSLog("[BSDisplayControl] IOKit setBrightness succeeded")
            return true
          }
        }
      }
    }
    return false
  }

  // MARK: - Display Name

  private func displayName(for displayID: CGDirectDisplayID) -> String {
    let mode = CoreGraphics.CGDisplayCopyDisplayMode(displayID)
    let width = mode?.width ?? 0
    let height = mode?.height ?? 0

    if CGDisplayIsBuiltin(displayID) != 0 {
      return "Built-in Display (\(width)x\(height))"
    }

    // Try IOKit to get the display product name, matching by vendor/product ID.
    let port: mach_port_t
    if #available(macOS 12.0, *) {
      port = kIOMainPortDefault
    } else {
      port = 0
    }

    let cgVendor = CGDisplayVendorNumber(displayID)
    let cgModel = CGDisplayModelNumber(displayID)

    var iterator: io_iterator_t = 0
    let result = IOServiceGetMatchingServices(
      port,
      IOServiceMatching("IODisplayConnect"),
      &iterator
    )
    if result == kIOReturnSuccess {
      defer { IOObjectRelease(iterator) }
      var service: io_object_t = IOIteratorNext(iterator)
      while service != 0 {
        defer {
          IOObjectRelease(service)
          service = IOIteratorNext(iterator)
        }

        if let infoDict = IODisplayCreateInfoDictionary(
             service,
             IOOptionBits(kIODisplayOnlyPreferredName)
           )?.takeRetainedValue() as? [String: Any] {
          let vendor = infoDict["DisplayVendorID"] as? UInt32 ?? 0
          let product = infoDict["DisplayProductID"] as? UInt32 ?? 0
          // Only return the name if vendor/product match this display.
          if vendor == cgVendor && product == cgModel,
             let names = infoDict["DisplayProductName"] as? [String: String],
             let name = names.values.first {
            return "\(name) (\(width)x\(height))"
          }
        }
      }
    }

    return "Display \(displayID) (\(width)x\(height))"
  }
}

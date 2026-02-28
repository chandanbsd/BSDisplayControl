# Windows Native Implementation

**Primary file:** `windows/runner/flutter_window.cpp` (431 lines)
**Header:** `windows/runner/flutter_window.h`
**Language:** C++17

This document covers how BSDisplayControl reads and writes monitor brightness on Windows.

## Architecture Overview

```
FlutterWindow::SetUpBrightnessChannel()
         |
         +-- "getDisplays"  --> MonitorEnumProc()
         |                       |
         |                       +-- GetMonitorBrightness()  [DDC/CI]
         |                       +-- GetWmiBrightness()      [WMI fallback]
         |
         +-- "setBrightness" --> SetMonitorBrightnessById()
                                  |
                                  +-- SetMonitorBrightness()  [DDC/CI]
                                  +-- SetWmiBrightness()      [WMI fallback]
```

## Display Enumeration (getDisplays)

When the Dart side calls `getDisplays`, the Windows implementation:

1. Calls `EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, ...)` -- a Win32 API that iterates over all active monitors and calls a callback for each one.

2. For each logical monitor (`HMONITOR`), it calls `GetPhysicalMonitorsFromHMONITOR()` to get physical monitor handles. A single logical monitor can have multiple physical monitors (e.g., in daisy-chain setups).

3. For each physical monitor, it reads the brightness via DDC/CI.

### Data Structures

```cpp
struct MonitorInfo {
  std::string id;          // Sequential index as string: "0", "1", "2"
  std::string name;        // From physical monitor description or device name
  double brightness;       // 0.0 - 1.0
  bool isBuiltIn;          // true if primary + single physical monitor
};

struct EnumContext {
  std::vector<MonitorInfo> monitors;  // Accumulated results
  int index;                          // Running counter
};
```

### Monitor ID Scheme

Windows uses a simple sequential integer index (`"0"`, `"1"`, `"2"`, ...) as the display ID. The index is the order in which `EnumDisplayMonitors` + `GetPhysicalMonitorsFromHMONITOR` yields monitors. This means IDs may change if monitors are connected/disconnected, but they're stable within a session.

### Display Name Resolution

Names come from two sources depending on the path taken:

1. **Physical monitor description** -- `PHYSICAL_MONITOR.szPhysicalMonitorDescription` (wide string). This is populated by the DDC/CI driver and usually contains the monitor model name (e.g., "DELL U2412M").

2. **Device name fallback** -- `MONITORINFOEXW.szDevice` (e.g., `\\.\DISPLAY1`). Used when physical monitor enumeration fails (no DDC/CI support).

### Built-in Detection

A monitor is flagged as `isBuiltIn` when:
- The `MONITORINFOF_PRIMARY` flag is set, AND
- There is exactly one physical monitor for this logical monitor (`physicalCount == 1`)

This heuristic works because laptops typically have one primary monitor that's the built-in panel.

## Brightness Control: DDC/CI Path

### Reading Brightness

```cpp
DWORD minBrightness = 0, curBrightness = 0, maxBrightness = 100;
GetMonitorBrightness(hPhysicalMonitor, &minBrightness, &curBrightness, &maxBrightness);

// Normalize to 0.0 - 1.0
double normalized = (double)(curBrightness - minBrightness) / (double)(maxBrightness - minBrightness);
```

The Win32 `GetMonitorBrightness()` function (from `Dxva2.lib`) wraps DDC/CI. It returns:
- `minBrightness`: the minimum brightness the monitor supports (usually 0)
- `curBrightness`: the current brightness
- `maxBrightness`: the maximum brightness (usually 100)

The app normalizes this to 0.0-1.0 using `(current - min) / (max - min)`.

### Writing Brightness

```cpp
DWORD newBrightness = minB + (maxB - minB) * clamp(brightness, 0.0, 1.0);
SetMonitorBrightness(hPhysicalMonitor, newBrightness);
```

The reverse: converts 0.0-1.0 back to the monitor's raw range and calls `SetMonitorBrightness()`.

### Required Libraries

| Library | Purpose |
| --- | --- |
| `Dxva2.lib` | `GetMonitorBrightness`, `SetMonitorBrightness`, `GetPhysicalMonitorsFromHMONITOR`, `DestroyPhysicalMonitors` |
| `dwmapi.lib` | Desktop Window Manager (used by Win32Window base class) |

## Brightness Control: WMI Fallback

Some laptop built-in displays don't support DDC/CI (`GetPhysicalMonitorsFromHMONITOR` returns 0 physical monitors, or `GetMonitorBrightness` fails). For these, the app falls back to **Windows Management Instrumentation (WMI)**.

### Reading via WMI (GetWmiBrightness)

The function queries the `ROOT\WMI` namespace:

```sql
SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE
```

This WMI class is provided by the display driver and works for integrated laptop panels that expose brightness through ACPI.

**COM initialization flow:**
1. `CoInitializeEx(COINIT_MULTITHREADED)` -- handles already-initialized case
2. `CoCreateInstance(CLSID_WbemLocator)` -- creates WMI locator
3. `ConnectServer("ROOT\\WMI")` -- connects to WMI namespace
4. `CoSetProxyBlanket()` -- sets security (impersonation)
5. `ExecQuery()` -- runs WQL query
6. Read `CurrentBrightness` variant from result
7. Cleanup all COM pointers in reverse order

### Writing via WMI (SetWmiBrightness)

Calls the `WmiSetBrightness` method on `WmiMonitorBrightnessMethods`:

1. Queries for `WmiMonitorBrightnessMethods WHERE Active=TRUE`
2. Gets the method definition: `pClass->GetMethod(L"WmiSetBrightness", ...)`
3. Spawns input params, sets `Timeout = 0` and `Brightness = value`
4. Gets the object path (`__PATH`) for the WMI instance
5. Calls `pServices->ExecMethod()` with the path and input params

**Required libraries:**
| Library | Purpose |
| --- | --- |
| `wbemuuid.lib` | WMI class IDs (`CLSID_WbemLocator`, `IID_IWbemLocator`) |
| `ole32.lib` | COM runtime (`CoInitializeEx`, `CoCreateInstance`) |
| `oleaut32.lib` | OLE Automation (`VARIANT`, `_bstr_t`) |

## SetMonitorBrightnessById

This function is called for `setBrightness`. It re-enumerates monitors (using `EnumDisplayMonitors`) with a lambda callback that counts monitors until it reaches the target index. Then:

1. If the target monitor has physical monitors (DDC/CI-capable): calls `SetMonitorBrightness()`.
2. If not (no DDC/CI, built-in laptop): calls `SetWmiBrightness()`.

The lambda returns `FALSE` (stop enumerating) once the target is found, or `TRUE` to continue.

## Windows-Specific Build Configuration

**`windows/runner/CMakeLists.txt`** links the necessary native libraries:

```cmake
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib")
target_link_libraries(${BINARY_NAME} PRIVATE "Dxva2.lib")
target_link_libraries(${BINARY_NAME} PRIVATE "wbemuuid.lib")
target_link_libraries(${BINARY_NAME} PRIVATE "ole32.lib")
target_link_libraries(${BINARY_NAME} PRIVATE "oleaut32.lib")
```

The `NOMINMAX` preprocessor definition prevents Windows headers from defining `min`/`max` macros that conflict with `<algorithm>`.

## Supporting Files

| File | Purpose |
| --- | --- |
| `main.cpp` | `wWinMain` entry point; initializes COM, creates `FlutterWindow`, runs message loop |
| `win32_window.h/.cpp` | Base Win32 window class with DPI awareness, dark mode theme detection |
| `utils.h/.cpp` | Console attachment (for `flutter run` output), UTF-16 to UTF-8 conversion |
| `Runner.rc` | Resource script: embeds app icon and version info |
| `runner.exe.manifest` | Declares PerMonitorV2 DPI awareness and Win10/11 compatibility |

## Limitations

1. **Monitor IDs are index-based** -- Unplugging a monitor shifts indices. This is acceptable since `getDisplays` is called on each app launch and refresh.

2. **WMI fallback is laptop-only** -- `WmiMonitorBrightness` only works for built-in panels with ACPI driver support. External monitors without DDC/CI cannot be controlled.

3. **No hot-plug detection** -- The app doesn't listen for `WM_DEVICECHANGE` or similar. Users must manually refresh to detect newly connected monitors.

4. **COM threading** -- The app initializes COM as `COINIT_APARTMENTTHREADED` in `main.cpp` but the WMI functions use `COINIT_MULTITHREADED`. The WMI functions handle this by checking if COM is already initialized (`S_FALSE` return from `CoInitializeEx`).

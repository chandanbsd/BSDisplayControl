# Platform Channel Bridge

This document explains how the Dart UI communicates with native platform code. This is the **central architectural concept** of the entire application.

## What Is a Platform Channel?

In web development, your Angular frontend calls your ASP.NET Core backend via HTTP. In Flutter desktop apps, the frontend (Dart) and backend (C++/Swift) run **in the same process**. They communicate via **Platform Channels** -- a binary messaging system built into Flutter's engine.

```
                    Same Process
+-------------------+     +--------------------+
|   Dart / Flutter   |     |   Native Code      |
|   (UI layer)       |     |   (C++ or Swift)   |
|                    |     |                    |
|  MethodChannel ----+---->+--- Handler         |
|    .invokeMethod() |     |    receives call,  |
|                    |     |    returns result   |
|  <-- result -------+<----+---                 |
+-------------------+     +--------------------+
```

**Key difference from HTTP:** There's no serialization to JSON. Flutter uses `StandardMethodCodec`, which serializes to a compact binary format. The Dart `Map<String, dynamic>` becomes a native `EncodableMap` (C++) or `[String: Any]` (Swift) directly.

## The BSDisplayControl Channel

| Property | Value |
| --- | --- |
| Channel name | `"com.bsdisplaycontrol/brightness"` |
| Codec | `StandardMethodCodec` (binary, not JSON) |
| Direction | Dart calls native (not bidirectional) |

### Registered Methods

There are exactly **two** methods on this channel:

---

### Method: `getDisplays`

**Purpose:** Enumerate all connected displays and read their current brightness.

**Request:**
- Method name: `"getDisplays"`
- Arguments: none

**Response:**
- Type: `List<Map>` (a list of display descriptor maps)
- Each map contains:

| Key | Type | Example | Description |
| --- | --- | --- | --- |
| `"id"` | String | `"drm:card1-DP-1"` | Platform-specific unique ID |
| `"name"` | String | `"DELL U2412M"` | Human-readable name (from EDID or OS) |
| `"brightness"` | double | `0.75` | Current brightness, 0.0-1.0 |
| `"isBuiltIn"` | bool | `false` | True for laptop panels |

**Platform-specific ID formats:**

| Platform | Built-in Example | External Example |
| --- | --- | --- |
| Windows | `"0"` (index) | `"1"` (index) |
| macOS | `"1"` (CGDirectDisplayID) | `"723664214"` (CGDirectDisplayID) |
| Linux | `"backlight"` | `"drm:card1-DP-1"` |

---

### Method: `setBrightness`

**Purpose:** Set the brightness of one specific display.

**Request:**
- Method name: `"setBrightness"`
- Arguments: `Map`

| Key | Type | Example | Description |
| --- | --- | --- | --- |
| `"displayId"` | String | `"drm:card1-DP-1"` | Must match an `id` from `getDisplays` |
| `"brightness"` | double | `0.75` | Target brightness, 0.0-1.0 |

**Response:**
- Type: `bool`
- `true` if the brightness was successfully set
- `false` if it failed (unsupported, permission denied, etc.)

---

## How Each Platform Registers the Channel

### Windows (C++)

In `windows/runner/flutter_window.cpp`, the channel is set up in `FlutterWindow::OnCreate()`:

```cpp
void FlutterWindow::SetUpBrightnessChannel() {
  auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(),
      "com.bsdisplaycontrol/brightness",
      &flutter::StandardMethodCodec::GetInstance());

  channel->SetMethodCallHandler(
      [](const flutter::MethodCall<flutter::EncodableValue>& call,
         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
        if (call.method_name() == "getDisplays") { ... }
        else if (call.method_name() == "setBrightness") { ... }
        else { result->NotImplemented(); }
      });
}
```

### macOS (Swift)

In `macos/Runner/MainFlutterWindow.swift`, the channel is set up in `awakeFromNib()`:

```swift
let channel = FlutterMethodChannel(
  name: "com.bsdisplaycontrol/brightness",
  binaryMessenger: flutterViewController.engine.binaryMessenger
)
let brightnessHandler = BrightnessMethodHandler()
channel.setMethodCallHandler(brightnessHandler.handle)
```

### Linux (C)

In `linux/runner/my_application.cc`, the channel is set up in `my_application_activate()`:

```c
g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
FlMethodChannel* brightness_channel = fl_method_channel_new(
    fl_engine_get_binary_messenger(fl_view_get_engine(view)),
    "com.bsdisplaycontrol/brightness",
    FL_METHOD_CODEC(codec));
fl_method_channel_set_method_call_handler(
    brightness_channel,
    brightness_method_call_handler,
    nullptr, nullptr);
```

## Error Handling

All three platforms follow the same error protocol:

| Scenario | Response |
| --- | --- |
| Unknown method name | `NotImplemented` (Flutter shows `MissingPluginException` on Dart side) |
| Missing/invalid arguments | `Error("INVALID_ARGS", "description")` |
| setBrightness succeeds | `Success(true)` |
| setBrightness fails | `Success(false)` (not an error -- the Dart side handles this gracefully) |
| getDisplays succeeds | `Success(List<Map>)` |

Note that `setBrightness` returning `false` is **not** a channel error -- it's a successful response indicating that the operation didn't work. The Dart side distinguishes between "channel call failed" (exception) and "operation returned false" (graceful failure).

## Data Type Mapping

The `StandardMethodCodec` handles type conversion across the Dart/native boundary:

| Dart Type | C++ Type (Flutter) | Swift Type | C Type (Linux) |
| --- | --- | --- | --- |
| `String` | `std::string` | `String` | `const gchar*` via `fl_value_get_string` |
| `double` | `double` | `Double` | `double` via `fl_value_get_float` |
| `bool` | `bool` | `Bool` | `gboolean` via `fl_value_get_bool` |
| `Map` | `flutter::EncodableMap` | `[String: Any]` | `FlValue` (FL_VALUE_TYPE_MAP) |
| `List` | `flutter::EncodableList` | `[Any]` | `FlValue` (FL_VALUE_TYPE_LIST) |

## Sequence Diagram: Full getDisplays Lifecycle

```
  HomeScreen            BrightnessService         MethodChannel        Native Handler
      |                       |                       |                     |
      |-- _loadDisplays() --> |                       |                     |
      |                       |-- invokeMethod ------>|                     |
      |                       |   ("getDisplays")     |-- binary msg ----->|
      |                       |                       |                     |
      |                       |                       |    [Native code     |
      |                       |                       |     enumerates      |
      |                       |                       |     monitors,       |
      |                       |                       |     reads DDC/CI,   |
      |                       |                       |     builds list]    |
      |                       |                       |                     |
      |                       |                       |<-- Success(list) ---|
      |                       |<-- Future<List> ------|                     |
      |                       |                       |                     |
      |<-- List<DisplayInfo> -|                       |                     |
      |                       |                       |                     |
      |-- setState() ------->|                       |                     |
      |   (updates UI)        |                       |                     |
```

## Sequence Diagram: Full setBrightness Lifecycle

```
  DisplayCard       HomeScreen          BrightnessService      MethodChannel     Native
      |                 |                      |                     |              |
      |-- onChanged --> |                      |                     |              |
      |   (value=0.5)   |                      |                     |              |
      |                 |-- setState() ------> |                     |              |
      |                 |   (optimistic UI)     |                     |              |
      |                 |                      |                     |              |
      |                 |-- [16ms debounce] --> |                     |              |
      |                 |                      |-- invokeMethod ---->|              |
      |                 |                      |  ("setBrightness",  |-- binary --->|
      |                 |                      |   {id, brightness}) |              |
      |                 |                      |                     |  [DDC/CI     |
      |                 |                      |                     |   command    |
      |                 |                      |                     |   to I2C/    |
      |                 |                      |                     |   Win32/     |
      |                 |                      |                     |   IOKit]     |
      |                 |                      |                     |              |
      |                 |                      |                     |<-- true -----|
      |                 |                      |<-- Future<bool> ----|              |
      |                 |<-- true -------------|                     |              |
      |                 |                      |                     |              |
```

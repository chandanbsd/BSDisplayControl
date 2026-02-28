# Dart/Flutter UI Layer

This document covers every Dart file in the `lib/` directory. The Dart layer is the **entire UI** of the application -- it renders the display list, sliders, and preset buttons. It knows nothing about how brightness is actually controlled; it delegates all hardware interaction to platform-native code via a `MethodChannel`.

## File Map

```
lib/
|-- main.dart                         # App entry point
|-- models/
|   +-- display_info.dart             # Data model
|-- services/
|   +-- brightness_service.dart       # Platform channel client
|-- screens/
|   +-- home_screen.dart              # Main screen (stateful)
+-- widgets/
    +-- display_brightness_card.dart   # Per-display card (stateless)
```

## main.dart

**Path:** `lib/main.dart` (35 lines)

This is the application entry point. It does three things:

1. Calls `runApp()` to launch the Flutter engine
2. Creates a `MaterialApp` with Material 3 theming
3. Sets `HomeScreen` as the root route

```dart
void main() {
  runApp(const BSDisplayControlApp());
}
```

### Theme Configuration

The app uses Material 3's `ColorScheme.fromSeed` to generate a cohesive color palette from a single seed color (`#1565C0`, a medium blue). It defines both light and dark themes and uses `ThemeMode.system` to automatically match the OS appearance setting.

**Web analogy:** This is like your Angular `AppModule` with a CSS theme variables file and `prefers-color-scheme` media query.

### Key Details

| Property | Value |
| --- | --- |
| App title | "BS Display Control" |
| Debug banner | Hidden (`debugShowCheckedModeBanner: false`) |
| Seed color | `#1565C0` (Material Blue 800) |
| Theme mode | `ThemeMode.system` (follows OS light/dark setting) |
| Material version | Material 3 (`useMaterial3: true`) |

---

## DisplayInfo Model

**Path:** `lib/models/display_info.dart` (87 lines)

This is the data transfer object (DTO) that represents a single physical display. It's the contract between the Dart UI and the native platform code.

**Web analogy:** This is like a TypeScript interface or a C# `record`:
```csharp
// C# equivalent
public record DisplayInfo(string Id, string Name, double Brightness, bool IsBuiltIn = false);
```

### Properties

| Property | Type | Description |
| --- | --- | --- |
| `id` | `String` | Platform-specific unique identifier. On Windows: numeric index (`"0"`, `"1"`). On macOS: `CGDirectDisplayID` as string. On Linux: `"backlight"` for built-in, `"drm:card1-DP-1"` for external. |
| `name` | `String` | Human-readable display name parsed from EDID data (e.g., `"DELL U2412M"`, `"Built-in Display (intel_backlight)"`). |
| `brightness` | `double` | Current brightness normalized to `0.0` - `1.0` range. All platforms normalize their raw values to this range before sending. |
| `isBuiltIn` | `bool` | `true` for laptop panels (eDP, LVDS), `false` for external monitors. Defaults to `false`. |

### Class Design

- **Immutable** -- All fields are `final`. The class is `final` (cannot be subclassed).
- **`const` constructor** -- Instances can be compile-time constants.
- **`copyWith`** -- Creates a modified copy (used for optimistic UI updates when the user drags a slider).
- **`fromMap` / `toMap`** -- Serialization for the `MethodChannel`. Flutter's `StandardMethodCodec` serializes Dart `Map`s to/from binary. The `fromMap` factory includes runtime type checks with descriptive error messages.
- **Equality by `id`** -- Two `DisplayInfo` instances are equal if they have the same `id`, regardless of name or brightness. This is important for list diffing.

### Serialization Format

The `Map` that crosses the platform channel:
```json
{
  "id": "drm:card1-DP-1",
  "name": "DELL U2412M",
  "brightness": 0.75,
  "isBuiltIn": false
}
```

---

## BrightnessService

**Path:** `lib/services/brightness_service.dart` (39 lines)

This is the **only point of contact** between the Dart UI and platform-native code. It wraps a `MethodChannel` and exposes two methods.

**Web analogy:** This is like an Angular `HttpClient` service, except instead of calling a REST API over HTTP, it sends binary messages to native C++/Swift code running in the same process.

```
// Angular equivalent
@Injectable({ providedIn: 'root' })
export class BrightnessService {
  getDisplays(): Observable<DisplayInfo[]> { ... }
  setBrightness(displayId: string, brightness: number): Observable<boolean> { ... }
}
```

### Singleton Pattern

The class uses Dart's factory constructor pattern for a singleton:
```dart
final class BrightnessService {
  BrightnessService._();                               // Private constructor
  static final BrightnessService instance = BrightnessService._();  // Single instance
}
```

### Channel Configuration

| Property | Value |
| --- | --- |
| Channel name | `"com.bsdisplaycontrol/brightness"` |
| Codec | `StandardMethodCodec` (default, binary serialization) |

### Methods

#### `getDisplays() -> Future<List<DisplayInfo>>`

Calls the native `getDisplays` method. Returns a list of all connected displays with their current brightness levels.

- Sends: no arguments
- Receives: `List<Map>` -- each map has `id`, `name`, `brightness`, `isBuiltIn`
- On null result: returns empty list
- Deserialization: each raw `Map<dynamic, dynamic>` is cast to `Map<String, dynamic>` then passed to `DisplayInfo.fromMap`

#### `setBrightness({displayId, brightness}) -> Future<bool>`

Sets the brightness for one specific display.

- Sends: `{ "displayId": String, "brightness": double }` (clamped to 0.0-1.0)
- Receives: `bool` -- `true` if the native code successfully set the brightness
- The `brightness` parameter is clamped **before** sending to prevent out-of-range values

---

## HomeScreen

**Path:** `lib/screens/home_screen.dart` (226 lines)

This is the main (and only) screen of the application. It's a `StatefulWidget` that manages the display list, loading state, error state, and brightness change debouncing.

**Web analogy:** This is a smart/container Angular component with `ngOnInit`, reactive state management, and child component delegation.

### State Fields

| Field | Type | Purpose |
| --- | --- | --- |
| `_brightnessService` | `BrightnessService` | Singleton service reference |
| `_displays` | `List<DisplayInfo>` | Current list of displays |
| `_isLoading` | `bool` | Loading spinner flag |
| `_error` | `String?` | Error message, null if no error |
| `_debounceTimer` | `Timer?` | Debounce timer for brightness changes |

### Lifecycle

1. **`initState()`** -- Calls `_loadDisplays()` on first render (like `ngOnInit`)
2. **`dispose()`** -- Cancels any pending debounce timer (like `ngOnDestroy`)

### _loadDisplays()

Async method that:
1. Sets `_isLoading = true`
2. Calls `BrightnessService.getDisplays()`
3. On success: stores display list, sets `_isLoading = false`
4. On error: stores error string, sets `_isLoading = false`
5. Checks `mounted` before calling `setState()` (prevents calling setState on a disposed widget -- equivalent to checking if a component is still alive before updating)

### _onBrightnessChanged()

Called when the user moves a slider or taps a preset button. Uses a **two-phase update strategy**:

1. **Immediate (optimistic):** Updates `_displays` list in-place via `copyWith()` so the slider moves instantly. No network/native call yet.

2. **Debounced (16ms):** After 16ms of no further changes, sends the actual `setBrightness` call to native code. If the call fails, shows a `SnackBar` error and reloads the real brightness values from hardware.

The 16ms debounce matches ~60fps, preventing I2C bus flooding during slider drags.

### UI Layout (build method)

Uses Dart 3's **switch expression** pattern matching on a tuple of `(_isLoading, _error)`:

```dart
body: switch ((_isLoading, _error)) {
  (true, _)                => CircularProgressIndicator,   // Loading
  (_, final String error)  => _ErrorView,                  // Error
  _ when _displays.isEmpty => _EmptyView,                  // No displays
  _                        => ListView.builder(...),       // Display list
},
```

| State | What Shows |
| --- | --- |
| Loading | Centered `CircularProgressIndicator` spinner |
| Error | Error icon, message text, "Retry" button |
| Empty list | Monitor icon, "No displays found" text, "Refresh" button |
| Has displays | Header text ("N displays detected") + list of `DisplayBrightnessCard` widgets |

### AppBar

- Title: "Display Control" (centered)
- Action: Refresh button (calls `_loadDisplays()`)

### Private Sub-Widgets

- **`_ErrorView`** -- Error state UI with icon, message, and retry button
- **`_EmptyView`** -- Empty state UI with monitor icon and refresh button

---

## DisplayBrightnessCard

**Path:** `lib/widgets/display_brightness_card.dart` (139 lines)

A stateless widget that renders a Material card for one display. It receives data and callbacks from its parent (`HomeScreen`) and has no internal state.

**Web analogy:** This is a dumb/presentational Angular component that receives `@Input()` properties and emits `@Output()` events.

### Props (Constructor Parameters)

| Parameter | Type | Description |
| --- | --- | --- |
| `display` | `DisplayInfo` | The display to render |
| `onBrightnessChanged` | `ValueChanged<double>` | Callback when brightness changes (like `@Output() EventEmitter<number>`) |

### Layout Structure

```
+-------------------------------------------------------+
| [Monitor Icon]  Display Name            [  75%  ]      |
|                 "Built-in Display"                      |
|                 (only if isBuiltIn)                     |
|                                                        |
| [sun-low] =====[========]=============== [sun-high]    |
|                    ^ slider                            |
|                                                        |
|   [ Dim 25% ]  [ Half 50% ]  [ Bright 75% ]  [ Max ]  |
|      ^             ^              ^              ^     |
|   preset buttons (highlight if active)                 |
+-------------------------------------------------------+
```

### Preset Buttons

Defined as a constant tuple list:

| Label | Icon | Value |
| --- | --- | --- |
| Dim | `brightness_low` | 0.25 (25%) |
| Half | `brightness_medium` | 0.50 (50%) |
| Bright | `brightness_high` | 0.75 (75%) |
| Max | `brightness_7` | 1.00 (100%) |

A preset is considered "active" (highlighted with the primary theme color) when the display's current brightness is within 1% of the preset value (`(display.brightness - value).abs() < 0.01`).

### Slider Configuration

| Property | Value |
| --- | --- |
| Range | 0.0 to 1.0 |
| Divisions | 100 (1% increments) |
| Label | Percentage shown on drag (`"75%"`) |
| Visual | Bracketed by sun-low and sun-high icons |

### Styling

- Material `Card` with elevation 2
- 16px horizontal margin, 8px vertical margin
- 20px internal padding
- Monitor icon (28px) with primary color
- Percentage badge: rounded container with `primaryContainer` background

---

## Test File

**Path:** `test/widget_test.dart` (16 lines)

A single smoke test that verifies:
1. The app renders without crashing
2. The "Display Control" title text is present
3. The refresh button icon exists

This is a basic regression safety net -- it doesn't test native brightness functionality (which requires actual hardware).

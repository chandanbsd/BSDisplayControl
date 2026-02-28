# Linux Native Implementation

**Primary file:** `linux/runner/my_application.cc` (865 lines)
**Header:** `linux/runner/my_application.h`
**Language:** C++17 (with C-style GTK/GLib APIs)

The Linux implementation is the most complex of the three platforms because Linux has no single standardized API for monitor brightness. This file implements everything from raw I2C bus communication to EDID binary parsing.

## Architecture Overview

```
my_application_activate()
    |
    +-- FlMethodChannel("com.bsdisplaycontrol/brightness")
         |
         +-- "getDisplays"
         |    |
         |    +-- FindBacklightPath()              [Built-in: /sys/class/backlight/]
         |    |   +-- GetBacklightBrightness()
         |    |
         |    +-- EnumerateDrmDisplays()            [External: /sys/class/drm/]
         |        +-- ParseEdidName()               [EDID binary parsing]
         |        +-- GetDisplayBrightness()
         |             |
         |             +-- 1. DdcGetBrightness()    [Direct I2C DDC/CI]
         |             +-- 2. DdcutilGetBrightness() [ddcutil CLI fallback]
         |             +-- 3. xrandr --verbose       [Software gamma fallback]
         |
         +-- "setBrightness"
              |
              +-- (id == "backlight")
              |    +-- SetBacklightBrightness()      [/sys/class/backlight/ or tee]
              |
              +-- (id == "drm:*")
                   +-- SetDisplayBrightness()
                        |
                        +-- 1. DdcSetBrightness()    [Direct I2C DDC/CI]
                        +-- 2. DdcutilSetBrightness() [ddcutil CLI fallback]
                        +-- 3. xrandr --brightness    [Software gamma fallback]
```

## Built-in Display: Backlight via sysfs

### Discovery (FindBacklightPath)

Linux exposes laptop backlight controls at `/sys/class/backlight/`. The function searches for known backlight interfaces in priority order:

| Priority | Name | Driver |
| --- | --- | --- |
| 1 | `intel_backlight` | Intel integrated graphics |
| 2 | `amdgpu_bl0` | AMD GPU |
| 3 | `amdgpu_bl1` | AMD GPU (secondary) |
| 4 | `acpi_video0` | Generic ACPI |
| 5 | (any) | First available |

### Reading (GetBacklightBrightness)

Reads two sysfs files and computes the normalized brightness:

```
/sys/class/backlight/<name>/brightness      -> current value (e.g., 750)
/sys/class/backlight/<name>/max_brightness   -> maximum value (e.g., 1000)

normalized = current / maximum  (e.g., 0.75)
```

### Writing (SetBacklightBrightness)

**Primary method:** Writes directly to the `brightness` file via `std::ofstream`. This works if the user has write permission (usually requires the `video` group).

**Fallback (tee):** If direct write fails (permission denied), uses `fork()`/`exec()` to pipe the value through the `tee` command:

```
echo "750" | tee /sys/class/backlight/.../brightness
```

This works because `tee` may have different permissions or capabilities. The implementation:
1. Creates a pipe (`pipe()`)
2. Forks a child process
3. Child: redirects stdin from pipe, stdout/stderr to `/dev/null`, execs `tee`
4. Parent: writes the brightness value to the pipe, closes it, waits for child

## External Monitors: DRM Enumeration

### DRM Connector Discovery (EnumerateDrmDisplays)

Scans `/sys/class/drm/` for connected display connectors:

```
/sys/class/drm/
    card1-DP-1/       <-- DisplayPort connector 1
        status        <-- "connected" or "disconnected"
        edid          <-- raw EDID binary (128+ bytes)
        i2c-10/       <-- I2C bus from graphics driver (subdirectory)
        ddc           <-- symlink to I2C bus for DDC (e.g., -> i2c-5)
    card1-DP-2/
    card1-HDMI-A-1/
    card1             <-- (skipped: no dash, it's the card itself)
    renderD128        <-- (skipped: no "card" prefix)
```

**Filtering rules:**
- Must start with `"card"`
- Must contain a dash (`-`) -- eliminates the bare `card1` entry
- Must not contain `"Writeback"` -- eliminates virtual connectors
- `status` file must read `"connected"`

### DrmDisplay Data Structure

```cpp
struct DrmDisplay {
  std::string connector;   // "card1-DP-1"
  std::string xrandrName;  // "DP-1"
  std::string edidName;    // "DELL U2412M"
  int i2cBus;              // From i2c-* subdirectory (e.g., 10), -1 if none
  int i2cBusDdc;           // From ddc symlink (e.g., 5), -1 if none
  bool isBuiltIn;          // true if eDP/LVDS/DSI connector
};
```

### DRM to xrandr Name Mapping

DRM connector names differ from xrandr output names:

| DRM Connector | xrandr Name | Transformation |
| --- | --- | --- |
| `card1-DP-1` | `DP-1` | Strip `card<N>-` prefix |
| `card1-HDMI-A-1` | `HDMI-1` | Strip prefix, remove `-A` from HDMI |
| `card1-eDP-1` | `eDP-1` | Strip prefix only |

### Built-in Detection

```cpp
disp.isBuiltIn = (xrandrName.find("eDP") == 0 ||   // Embedded DisplayPort (laptops)
                  xrandrName.find("LVDS") == 0 ||   // Legacy laptop connector
                  xrandrName.find("DSI") == 0);      // Display Serial Interface (tablets)
```

## I2C Bus Discovery

Each DRM connector may expose **two** I2C buses:

1. **Subdirectory bus (`i2c-*`):** A directory like `card1-DP-1/i2c-10/`. This is the I2C adapter registered by the GPU driver's display engine.

2. **DDC symlink (`ddc`):** A symlink like `card1-DP-1/ddc -> ../../i2c-5`. This is the bus specifically designated for DDC communication.

**Critical finding from real hardware testing:** Not all buses work for DDC/CI on all connectors. The working bus varies per connector:

| Connector | i2c subdir | ddc symlink | Working Bus |
| --- | --- | --- | --- |
| card1-DP-1 | i2c-10 | i2c-5 | i2c-10 (subdir) |
| card1-DP-2 | i2c-11 | i2c-6 | i2c-6 (ddc symlink) |
| card1-DP-3 | i2c-12 | i2c-7 | i2c-12 (subdir) |
| card1-HDMI-A-1 | (none) | i2c-8 | i2c-8 (ddc) |

**Solution:** The code stores both bus numbers and tries both when reading/setting brightness.

## EDID Parsing (ParseEdidName)

EDID (Extended Display Identification Data) is a 128-byte binary structure embedded in every monitor's firmware. The app reads it from `/sys/class/drm/<connector>/edid`.

### Binary Format Parsed

```
Offset  Field
------  -----
 8-9    Manufacturer ID (compressed ASCII, 3 letters)
54-71   Descriptor Block 0
72-89   Descriptor Block 1
90-107  Descriptor Block 2
108-125 Descriptor Block 3
```

### Manufacturer ID Decoding

Bytes 8-9 encode three ASCII letters in 15 bits (5 bits each, offset by 64):

```cpp
uint16_t mfr = (data[8] << 8) | data[9];
char c1 = ((mfr >> 10) & 0x1F) + 64;  // e.g., 'D'
char c2 = ((mfr >>  5) & 0x1F) + 64;  // e.g., 'E'
char c3 = ((mfr      ) & 0x1F) + 64;  // e.g., 'L'
// manufacturer = "DEL" (Dell)
```

### Monitor Name Extraction

Each descriptor block at offsets 54, 72, 90, 108 is checked for tag `0xFC` (Monitor Name):

```
Offset+0: 0x00 (flag byte 0)
Offset+1: 0x00 (flag byte 1)
Offset+3: 0xFC (tag: Monitor Name)
Offset+5 to Offset+17: ASCII name (terminated by \n or \0)
```

If no monitor name descriptor is found, the manufacturer code (e.g., "DEL") is returned as fallback.

## DDC/CI Protocol (Direct I2C)

DDC/CI (Display Data Channel / Command Interface) is a protocol for sending commands to monitors over the I2C bus. The app implements this protocol directly using Linux's I2C device driver.

### Constants

```cpp
DDC_CI_ADDR = 0x37       // I2C slave address for DDC/CI
VCP_BRIGHTNESS = 0x10    // VCP (Virtual Control Panel) code for brightness
```

### Get Brightness (DdcGetBrightness)

**Step 1: Open I2C device and set slave address**
```cpp
int fd = open("/dev/i2c-10", O_RDWR);
ioctl(fd, I2C_SLAVE, 0x37);
```

**Step 2: Send "Get VCP Feature" request**
```
Byte:  [0x51] [0x82] [0x01] [0x10] [checksum]
        |      |      |      |      |
        |      |      |      |      +-- XOR checksum of 0x6E ^ all preceding bytes
        |      |      |      +-- VCP code (0x10 = Brightness)
        |      |      +-- Get VCP Feature opcode
        |      +-- Length (0x80 | 2 = 2 data bytes following)
        +-- Destination address (0x51 = host-to-display)
```

**Step 3: Wait 50ms** (DDC/CI spec requires 40-50ms for monitor to process)

**Step 4: Read response (up to 12 bytes)**

**Step 5: Parse VCP Feature Reply**
```
Find bytes where: response[i] == 0x02 (VCP reply opcode)
                  response[i+2] == 0x10 (brightness VCP code)

Response format from offset i:
[0x02] [result] [0x10] [type] [max_hi] [max_lo] [cur_hi] [cur_lo]
  i      i+1     i+2    i+3     i+4     i+5      i+6      i+7

result: 0x00 = success, non-zero = error
type:   0x00 = set parameter, 0x01 = momentary
max:    (max_hi << 8) | max_lo  (typically 100)
cur:    (cur_hi << 8) | cur_lo  (e.g., 75)
```

**Note:** The TYPE byte at offset +3 is critical. An earlier bug in this code skipped it, reading max and current from wrong offsets, causing `max=0` and `current=25600`.

### Set Brightness (DdcSetBrightness)

**Send "Set VCP Feature" command:**
```
Byte:  [0x51] [0x84] [0x03] [0x10] [val_hi] [val_lo] [checksum]
        |      |      |      |      |         |         |
        |      |      |      |      +---------+-- brightness value (big-endian)
        |      |      |      +-- VCP code (brightness)
        |      |      +-- Set VCP Feature opcode
        |      +-- Length (0x80 | 4 = 4 data bytes)
        +-- Destination address
```

### DDC/CI Checksum

```cpp
uint8_t DdcChecksum(uint8_t srcAddr, const uint8_t* data, size_t len) {
  uint8_t csum = srcAddr;  // Start with source address (0x6E for host)
  for (size_t i = 0; i < len; ++i) csum ^= data[i];
  return csum;
}
```

## I2C Permission Management (SetupI2cPermissions)

`/dev/i2c-*` devices are typically owned by `root:root` with mode `0600`. The app needs read/write access. This is handled by a two-step permission setup:

### Step 1: Immediate Access (pkexec)

```cpp
system("pkexec chmod 0666 /dev/i2c-*");
```

This uses PolicyKit's `pkexec` to show a GUI password prompt and run `chmod` as root. This grants immediate access but doesn't survive reboots.

### Step 2: Persistent Setup (udev rule)

A shell script is written to `/tmp` and executed via `pkexec`:

```bash
#!/bin/sh
grep -q '^i2c:' /etc/group || groupadd i2c
usermod -aG i2c $USER
echo 'KERNEL=="i2c-[0-9]*", GROUP="i2c", MODE="0666"' > /etc/udev/rules.d/99-i2c-permissions.rules
udevadm control --reload-rules 2>/dev/null
udevadm trigger --subsystem-match=i2c-dev 2>/dev/null
```

This creates:
1. An `i2c` group (if it doesn't exist)
2. Adds the current user to the `i2c` group
3. A udev rule that sets permissions on I2C devices at boot
4. Reloads udev rules and re-triggers device events

**Note:** The udev rule uses `MODE="0666"` (world-readable/writable) rather than `MODE="0660"` (group-only) because the running process may not have the new group membership until the next login.

### Guard Variables

```cpp
static bool g_i2c_setup_attempted = false;  // Only try once per session
static bool g_i2c_accessible = false;       // Cached result
```

## ddcutil CLI Fallback

If direct I2C DDC/CI fails, the app tries the `ddcutil` command-line tool (if installed):

### Get Brightness
```bash
ddcutil getvcp 10 --bus 10 --brief 2>/dev/null
# Output: "VCP 10 C 75 100"
#          VCP code   current max
```

### Set Brightness
```bash
ddcutil setvcp 10 75 --bus 10 --noverify 2>/dev/null
```

The `--noverify` flag skips re-reading the value after setting, which is faster and avoids timeout issues.

`ddcutil` availability is checked once and cached:
```cpp
static bool g_ddcutil_checked = false;
static bool g_ddcutil_available = false;
```

## xrandr Software Gamma Fallback

As a last resort (no I2C access, no ddcutil), the app uses `xrandr --brightness` which adjusts the **software gamma curve**, not the actual backlight:

### Reading
```bash
xrandr --verbose | grep -A15 '^DP-1 connected'
# Look for "Brightness: 0.75" in output
```

### Writing
```bash
xrandr --output DP-1 --brightness 0.75
```

**Important:** This is NOT real brightness control. It multiplies the gamma LUT, which:
- Washes out colors at low values
- Cannot go below the monitor's minimum backlight
- Only works on X11 (not Wayland -- though xrandr may partially work via XWayland)

## Method Channel Handler

The `brightness_method_call_handler` function dispatches platform channel calls:

### getDisplays Flow

1. Find built-in backlight (`FindBacklightPath`)
2. If found, add it as `id = "backlight"`, `isBuiltIn = true`
3. Enumerate DRM displays (`EnumerateDrmDisplays`)
4. For each DRM display:
   - Skip if it's built-in AND we already have a backlight entry
   - Read brightness via `GetDisplayBrightness()` (tries all fallbacks)
   - Create a display map with `id = "drm:<connector>"`
5. Return the combined list

### setBrightness Flow

1. Parse `displayId` and `brightness` from arguments
2. If `displayId == "backlight"`: call `SetBacklightBrightness()`
3. Otherwise: search the cached `g_drmDisplays` list for a matching `"drm:<connector>"`
4. Call `SetDisplayBrightness()` on the matched display

### Display List Caching

```cpp
static std::vector<DrmDisplay> g_drmDisplays;
```

The DRM display list is cached globally and refreshed on each `getDisplays` call. The `setBrightness` handler uses this cached list to look up displays by ID, avoiding re-enumeration on every brightness change.

## GTK Application Boilerplate

The file also contains the standard Flutter-Linux GTK application setup:

- **`_MyApplication` struct** -- GLib object containing `dart_entrypoint_arguments`
- **`my_application_activate()`** -- Creates GTK window, Flutter view, registers method channel
- **Header bar** -- Uses GTK header bar on GNOME Shell, traditional title bar otherwise
- **Window size** -- 800x600 default
- **Background** -- Black (`#000000`)

## Linux Build Configuration

### CMake Toolchain Workaround

`linux/CMakeLists.txt` contains a workaround for Ubuntu systems where `clang++` is the default compiler but only older GCC libraries are installed:

```cmake
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  file(GLOB _gcc_vers "/usr/lib/gcc/x86_64-linux-gnu/*")
  foreach(_dir ${_gcc_vers})
    if(IS_DIRECTORY ${_dir} AND EXISTS "${_dir}/libstdc++.so")
      set(_gcc_install_dir "${_dir}")
    endif()
  endforeach()
  # Point clang at the found GCC installation
  set(CMAKE_CXX_FLAGS "--gcc-install-dir=${_gcc_install_dir}")
  set(CMAKE_EXE_LINKER_FLAGS "-L${_gcc_install_dir}")
endif()
```

### Linked Libraries

`linux/runner/CMakeLists.txt`:
```cmake
target_link_libraries(${BINARY_NAME} PRIVATE flutter)
target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::GTK)
```

No additional libraries are needed beyond Flutter and GTK because:
- I2C access uses standard Linux kernel ioctls (`<linux/i2c-dev.h>`, `<linux/i2c.h>`)
- File I/O uses `<fstream>` and POSIX `open()`/`read()`/`write()`
- Process management uses POSIX `fork()`/`exec()`/`waitpid()`

### System Headers Used

| Header | Purpose |
| --- | --- |
| `<linux/i2c-dev.h>` | `I2C_SLAVE` ioctl |
| `<linux/i2c.h>` | I2C data structures |
| `<sys/ioctl.h>` | `ioctl()` system call |
| `<fcntl.h>` | `open()`, `O_RDWR` |
| `<unistd.h>` | `read()`, `write()`, `close()`, `fork()`, `exec()` |
| `<sys/wait.h>` | `waitpid()` |
| `<filesystem>` | C++17 filesystem (directory iteration, symlink resolution) |

## Limitations

1. **Wayland support is partial** -- The xrandr gamma fallback only works on X11. On pure Wayland, if DDC/CI also fails, there's no fallback. However, DDC/CI (the primary method) works on both X11 and Wayland since it bypasses the display server entirely.

2. **I2C permissions require user interaction** -- The first launch prompts for a password via `pkexec`. If the user cancels, brightness control won't work for external monitors.

3. **No hot-plug detection** -- Like the other platforms, the app doesn't automatically detect newly connected monitors.

4. **Process forking** -- The `SetBacklightBrightness` tee fallback and `xrandr` fallback use `fork()`/`exec()`, which is unusual in a GUI application. This works but is heavier than direct system calls.

5. **Single-architecture toolchain workaround** -- The CMake GCC detection only handles `x86_64-linux-gnu`. ARM64 or other architectures would need their own paths.

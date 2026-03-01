#include "my_application.h"

#include <flutter_linux/flutter_linux.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#include "flutter/generated_plugin_registrant.h"

// ── Utility: run a command and capture stdout ──────────────────────

static std::string RunCommand(const std::string& cmd) {
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return "";
  std::string result;
  char buf[256];
  while (fgets(buf, sizeof(buf), pipe)) {
    result += buf;
  }
  pclose(pipe);
  return result;
}

// ── Utility: check if a command exists ─────────────────────────────

static bool CommandExists(const std::string& cmd) {
  std::string check = "which " + cmd + " >/dev/null 2>&1";
  return system(check.c_str()) == 0;
}

// ── EDID parsing: extract monitor name from raw EDID ───────────────

static std::string ParseEdidName(const std::string& edidPath) {
  std::ifstream file(edidPath, std::ios::binary);
  if (!file.is_open()) return "";

  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  if (data.size() < 128) return "";

  // Parse manufacturer ID from bytes 8-9 (compressed ASCII).
  uint16_t mfr = (static_cast<uint16_t>(data[8]) << 8) | data[9];
  char c1 = static_cast<char>(((mfr >> 10) & 0x1F) + 64);
  char c2 = static_cast<char>(((mfr >> 5) & 0x1F) + 64);
  char c3 = static_cast<char>((mfr & 0x1F) + 64);
  std::string manufacturer = {c1, c2, c3};

  // Parse descriptor blocks at offsets 54, 72, 90, 108 for monitor name (tag 0xFC).
  std::string monitorName;
  for (int off : {54, 72, 90, 108}) {
    if (off + 18 > static_cast<int>(data.size())) break;
    if (data[off] == 0 && data[off + 1] == 0 && data[off + 3] == 0xFC) {
      for (int i = 5; i < 18; ++i) {
        char ch = static_cast<char>(data[off + i]);
        if (ch == '\n' || ch == '\0') break;
        monitorName += ch;
      }
      break;
    }
  }

  // Trim whitespace.
  while (!monitorName.empty() && monitorName.back() == ' ')
    monitorName.pop_back();

  if (!monitorName.empty()) return monitorName;
  return manufacturer;  // Fallback to manufacturer code.
}

// ── Brightness control via sysfs (backlight) ───────────────────────

static std::string FindBacklightPath() {
  const std::string basePath = "/sys/class/backlight";
  if (!std::filesystem::exists(basePath)) return "";

  std::vector<std::string> preferred = {
    "intel_backlight", "amdgpu_bl0", "amdgpu_bl1", "acpi_video0"
  };
  for (const auto& name : preferred) {
    auto path = basePath + "/" + name;
    if (std::filesystem::exists(path)) return path;
  }

  for (const auto& entry : std::filesystem::directory_iterator(basePath)) {
    return entry.path().string();
  }
  return "";
}

static double GetBacklightBrightness(const std::string& backlightPath) {
  std::ifstream curFile(backlightPath + "/brightness");
  std::ifstream maxFile(backlightPath + "/max_brightness");
  if (!curFile.is_open() || !maxFile.is_open()) return 1.0;

  int current = 0, maximum = 1;
  curFile >> current;
  maxFile >> maximum;
  if (maximum <= 0) return 1.0;
  return static_cast<double>(current) / static_cast<double>(maximum);
}

static bool SetBacklightBrightness(const std::string& backlightPath, double brightness) {
  std::ifstream maxFile(backlightPath + "/max_brightness");
  if (!maxFile.is_open()) return false;

  int maximum = 0;
  maxFile >> maximum;
  if (maximum <= 0) return false;

  int newValue = static_cast<int>(std::clamp(brightness, 0.0, 1.0) * maximum);
  if (newValue < 1 && brightness > 0.0) newValue = 1;

  std::ofstream curFile(backlightPath + "/brightness");
  if (curFile.is_open()) {
    curFile << newValue;
    return curFile.good();
  }

  // Fallback: use fork/exec with tee for permission issues.
  std::string brightnessFile = backlightPath + "/brightness";
  std::string valueStr = std::to_string(newValue);

  int pipefd[2];
  if (pipe(pipefd) != 0) return false;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }

  if (pid == 0) {
    close(pipefd[1]);
    dup2(pipefd[0], STDIN_FILENO);
    close(pipefd[0]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("tee", "tee", brightnessFile.c_str(), nullptr);
    _exit(1);
  }

  close(pipefd[0]);
  write(pipefd[1], valueStr.c_str(), valueStr.size());
  close(pipefd[1]);

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// ── DDC/CI via direct I2C ──────────────────────────────────────────
//
// DDC/CI uses I2C address 0x37.  VCP code 0x10 = Brightness.
// Protocol: write [0x51, 0x84, 0x03, 0x01, 0x10, 0x00, checksum]
//           to get current value; parse the response.

static const uint8_t DDC_CI_ADDR = 0x37;
static const uint8_t VCP_BRIGHTNESS = 0x10;

// Compute DDC/CI checksum: XOR of (source_addr << 1) and all payload bytes.
// ── I2C permission setup ───────────────────────────────────────────
//
// DDC/CI requires read/write access to /dev/i2c-* devices.  On most Linux
// distros these are root-only by default.  This helper sets up the required
// udev rule and user group so the app can access I2C without root.
// It runs once and uses pkexec (PolicyKit) to get the needed privileges.

static bool g_i2c_setup_attempted = false;
static bool g_i2c_accessible = false;

static bool SetupI2cPermissions() {
  if (g_i2c_setup_attempted) return g_i2c_accessible;
  g_i2c_setup_attempted = true;

  // Check if i2c-dev module is loaded; load it if not.
  if (!std::filesystem::exists("/dev/i2c-0")) {
    system("modprobe i2c-dev 2>/dev/null");
  }

  // Check if we already have access to any I2C device.
  for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
    std::string name = entry.path().filename().string();
    if (name.find("i2c-") == 0) {
      int fd = open(entry.path().c_str(), O_RDWR);
      if (fd >= 0) {
        close(fd);
        g_i2c_accessible = true;
        fprintf(stderr, "[BSDisplayControl] I2C devices already accessible.\n");
        return true;
      }
      break;  // If one is inaccessible, they all are.
    }
  }

  // I2C devices exist but are not accessible.
  fprintf(stderr, "[BSDisplayControl] I2C devices not accessible, requesting permissions...\n");

  std::string user = getenv("USER") ? getenv("USER") : "";

  // Step 1: Immediately grant access with chmod 0666 (simple, reliable pkexec call).
  int ret = system("pkexec chmod 0666 /dev/i2c-*");
  if (ret != 0) {
    fprintf(stderr, "[BSDisplayControl] pkexec chmod failed (ret=%d).\n", ret);
    fprintf(stderr, "[BSDisplayControl] You can set up manually:\n");
    fprintf(stderr, "  sudo chmod 0666 /dev/i2c-*\n");
    fprintf(stderr, "  (For permanent fix: sudo usermod -aG i2c $USER and reboot)\n");
    return false;
  }

  // Step 2: Set up persistent udev rule + i2c group in background (best-effort).
  // This makes permissions survive reboots. Failures here are non-fatal.
  if (!user.empty()) {
    // Write a small script to /tmp and execute it via pkexec.
    std::string scriptPath = "/tmp/bsdisplaycontrol_i2c_setup.sh";
    {
      std::ofstream script(scriptPath);
      script << "#!/bin/sh\n"
             << "grep -q '^i2c:' /etc/group || groupadd i2c\n"
             << "usermod -aG i2c " << user << "\n"
             << "echo 'KERNEL==\"i2c-[0-9]*\", GROUP=\"i2c\", MODE=\"0666\"' "
             << "> /etc/udev/rules.d/99-i2c-permissions.rules\n"
             << "udevadm control --reload-rules 2>/dev/null\n"
             << "udevadm trigger --subsystem-match=i2c-dev 2>/dev/null\n";
    }
    chmod(scriptPath.c_str(), 0755);

    // Run the persistent setup via pkexec (separate prompt, or may be cached).
    std::string setupCmd = "pkexec " + scriptPath + " 2>/dev/null";
    int setupRet = system(setupCmd.c_str());
    if (setupRet != 0) {
      fprintf(stderr, "[BSDisplayControl] Persistent udev setup failed (non-fatal).\n");
      fprintf(stderr, "[BSDisplayControl] Permissions will reset on reboot.\n");
    } else {
      fprintf(stderr, "[BSDisplayControl] Persistent I2C permissions installed.\n");
    }
    unlink(scriptPath.c_str());
  }

  // Verify access.
  for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
    std::string name = entry.path().filename().string();
    if (name.find("i2c-") == 0) {
      int fd = open(entry.path().c_str(), O_RDWR);
      if (fd >= 0) {
        close(fd);
        g_i2c_accessible = true;
        fprintf(stderr, "[BSDisplayControl] I2C permissions set up successfully.\n");
        return true;
      }
      break;
    }
  }

  fprintf(stderr, "[BSDisplayControl] I2C still not accessible after setup.\n");
  return false;
}

// ── DDC/CI protocol ────────────────────────────────────────────────

static uint8_t DdcChecksum(uint8_t srcAddr, const uint8_t* data, size_t len) {
  uint8_t csum = srcAddr;
  for (size_t i = 0; i < len; ++i) csum ^= data[i];
  return csum;
}

// Try to get brightness via DDC/CI on a given I2C bus.
// Returns true if successful, with brightness in [0..100].
static bool DdcGetBrightness(int busNum, int& outCurrent, int& outMax) {
  std::string devPath = "/dev/i2c-" + std::to_string(busNum);
  int fd = open(devPath.c_str(), O_RDWR);
  if (fd < 0) return false;

  if (ioctl(fd, I2C_SLAVE, DDC_CI_ADDR) < 0) {
    close(fd);
    return false;
  }

  // DDC/CI "Get VCP Feature" request for brightness.
  uint8_t request[] = {0x51, 0x82, 0x01, VCP_BRIGHTNESS, 0x00};
  request[4] = DdcChecksum(0x6E, request, 4);

  if (write(fd, request, sizeof(request)) != static_cast<ssize_t>(sizeof(request))) {
    close(fd);
    return false;
  }

  // DDC/CI spec says to wait 40-50ms for the monitor to respond.
  usleep(50000);

  uint8_t response[12] = {};
  ssize_t bytesRead = read(fd, response, sizeof(response));
  close(fd);

  if (bytesRead < 9) return false;

  // Find the VCP Feature Reply opcode (0x02) in the response.
  // Format: [opcode=0x02][result][vcp_code][type][max_hi][max_lo][cur_hi][cur_lo]
  int offset = -1;
  for (int i = 0; i < bytesRead - 8; ++i) {
    if (response[i] == 0x02 && response[i + 2] == VCP_BRIGHTNESS) {
      offset = i;
      break;
    }
  }
  if (offset < 0) return false;

  if (response[offset + 1] != 0x00) return false;  // Result code error.

  // offset+3 = VCP type code (skip it).
  outMax = (response[offset + 4] << 8) | response[offset + 5];
  outCurrent = (response[offset + 6] << 8) | response[offset + 7];

  if (outMax <= 0) return false;
  return true;
}

// Set brightness via DDC/CI on a given I2C bus.
static bool DdcSetBrightness(int busNum, int value) {
  std::string devPath = "/dev/i2c-" + std::to_string(busNum);
  int fd = open(devPath.c_str(), O_RDWR);
  if (fd < 0) return false;

  if (ioctl(fd, I2C_SLAVE, DDC_CI_ADDR) < 0) {
    close(fd);
    return false;
  }

  uint8_t valueHi = static_cast<uint8_t>((value >> 8) & 0xFF);
  uint8_t valueLo = static_cast<uint8_t>(value & 0xFF);
  uint8_t cmd[] = {0x51, 0x84, 0x03, VCP_BRIGHTNESS, valueHi, valueLo, 0x00};
  cmd[6] = DdcChecksum(0x6E, cmd, 6);

  ssize_t written = write(fd, cmd, sizeof(cmd));
  close(fd);

  return written == static_cast<ssize_t>(sizeof(cmd));
}

// ── DDC/CI via ddcutil command-line (fallback) ─────────────────────

static bool g_ddcutil_checked = false;
static bool g_ddcutil_available = false;

static bool IsDdcutilAvailable() {
  if (!g_ddcutil_checked) {
    g_ddcutil_available = CommandExists("ddcutil");
    g_ddcutil_checked = true;
  }
  return g_ddcutil_available;
}

// Get brightness using ddcutil for a specific I2C bus.
// Returns true if successful, with brightness 0-100.
static bool DdcutilGetBrightness(int busNum, int& outCurrent, int& outMax) {
  if (!IsDdcutilAvailable()) return false;

  char cmd[128];
  snprintf(cmd, sizeof(cmd),
           "ddcutil getvcp 10 --bus %d --brief 2>/dev/null", busNum);
  std::string output = RunCommand(cmd);

  // Brief format: "VCP 10 C <current> <max>"
  int current = 0, maximum = 0;
  if (sscanf(output.c_str(), "VCP %*x C %d %d", &current, &maximum) == 2 && maximum > 0) {
    outCurrent = current;
    outMax = maximum;
    return true;
  }
  return false;
}

static bool DdcutilSetBrightness(int busNum, int value) {
  if (!IsDdcutilAvailable()) return false;

  char cmd[128];
  snprintf(cmd, sizeof(cmd),
           "ddcutil setvcp 10 %d --bus %d --noverify 2>/dev/null", value, busNum);
  return system(cmd) == 0;
}

// ── DRM-based display enumeration ──────────────────────────────────
//
// Enumerate connected displays by scanning /sys/class/drm/card*-*/.
// For each connected connector:
//   - Read EDID for human-readable name
//   - Find the associated I2C bus (via i2c-* subdirectory or ddc symlink)
//   - Map DRM connector name (e.g. "card1-DP-1") to xrandr name (e.g. "DP-1")

struct DrmDisplay {
  std::string connector;   // e.g., "card1-DP-1"
  std::string xrandrName;  // e.g., "DP-1"
  std::string edidName;    // e.g., "DELL U2412M"
  int i2cBus;              // Primary I2C bus (from i2c-* subdir), -1 if N/A
  int i2cBusDdc;           // Secondary I2C bus (from ddc symlink), -1 if N/A
  bool isBuiltIn;
};

static std::string DrmConnectorToXrandr(const std::string& connector) {
  // DRM connector: "card1-DP-1", "card1-HDMI-A-1"
  // xrandr name:   "DP-1",       "HDMI-1"
  auto dashPos = connector.find('-');
  if (dashPos == std::string::npos) return connector;
  std::string name = connector.substr(dashPos + 1);  // "DP-1" or "HDMI-A-1"

  // HDMI-A-1 -> HDMI-1 (xrandr drops the "-A")
  auto hdmiA = name.find("HDMI-A-");
  if (hdmiA != std::string::npos) {
    name = "HDMI-" + name.substr(7);
  }
  return name;
}

static std::vector<DrmDisplay> EnumerateDrmDisplays() {
  std::vector<DrmDisplay> displays;
  const std::string drmBase = "/sys/class/drm";

  if (!std::filesystem::exists(drmBase)) return displays;

  for (const auto& entry : std::filesystem::directory_iterator(drmBase)) {
    std::string dirname = entry.path().filename().string();
    // Only look at connector entries like "card1-DP-1", not "card1" or "renderD128".
    if (dirname.find("card") != 0) continue;
    if (dirname.find('-') == std::string::npos) continue;
    if (dirname.find("Writeback") != std::string::npos) continue;

    std::string statusPath = entry.path().string() + "/status";
    std::ifstream statusFile(statusPath);
    if (!statusFile.is_open()) continue;
    std::string status;
    std::getline(statusFile, status);
    if (status != "connected") continue;

    DrmDisplay disp;
    disp.connector = dirname;
    disp.xrandrName = DrmConnectorToXrandr(dirname);
    disp.i2cBus = -1;
    disp.i2cBusDdc = -1;

    // Check if this is a built-in display.
    disp.isBuiltIn = (disp.xrandrName.find("eDP") == 0 ||
                      disp.xrandrName.find("LVDS") == 0 ||
                      disp.xrandrName.find("DSI") == 0);

    // Read EDID for display name.
    std::string edidPath = entry.path().string() + "/edid";
    disp.edidName = ParseEdidName(edidPath);

    // Find I2C bus: look for i2c-* subdirectory first, then ddc symlink.
    for (const auto& sub : std::filesystem::directory_iterator(entry.path())) {
      std::string subname = sub.path().filename().string();
      if (subname.find("i2c-") == 0) {
        try {
          disp.i2cBus = std::stoi(subname.substr(4));
        } catch (...) {}
        break;
      }
    }

    if (disp.i2cBus < 0) {
      // Check for "ddc" symlink (common for HDMI).
      std::string ddcLink = entry.path().string() + "/ddc";
      if (std::filesystem::is_symlink(ddcLink)) {
        std::string target = std::filesystem::read_symlink(ddcLink).filename().string();
        if (target.find("i2c-") == 0) {
          try {
            disp.i2cBus = std::stoi(target.substr(4));
          } catch (...) {}
        }
      }
    } else {
      // We have an i2c-* subdir bus; also check for "ddc" symlink as fallback.
      std::string ddcLink = entry.path().string() + "/ddc";
      if (std::filesystem::is_symlink(ddcLink)) {
        std::string target = std::filesystem::read_symlink(ddcLink).filename().string();
        if (target.find("i2c-") == 0) {
          try {
            disp.i2cBusDdc = std::stoi(target.substr(4));
          } catch (...) {}
        }
      }
    }

    displays.push_back(disp);
  }

  return displays;
}

// ── Get brightness for a display (try DDC/CI, then ddcutil, then xrandr) ──
// Try both I2C buses (primary from i2c-* subdir, fallback from ddc symlink).

static double GetDisplayBrightness(const DrmDisplay& disp) {
  // Collect candidate I2C buses to try.
  std::vector<int> buses;
  if (disp.i2cBus >= 0) buses.push_back(disp.i2cBus);
  if (disp.i2cBusDdc >= 0 && disp.i2cBusDdc != disp.i2cBus)
    buses.push_back(disp.i2cBusDdc);

  if (!buses.empty()) {
    // Ensure I2C permissions are set up (one-time, prompts user if needed).
    if (!g_i2c_accessible && !g_i2c_setup_attempted) {
      SetupI2cPermissions();
    }

    for (int bus : buses) {
      int current = 0, maximum = 100;
      // Try direct I2C DDC/CI.
      if (DdcGetBrightness(bus, current, maximum)) {
        return static_cast<double>(current) / static_cast<double>(maximum);
      }
      // Try ddcutil as fallback.
      if (DdcutilGetBrightness(bus, current, maximum)) {
        return static_cast<double>(current) / static_cast<double>(maximum);
      }
    }
  }

  // Fallback: try xrandr verbose output for this specific output.
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "xrandr --verbose 2>/dev/null | grep -A15 '^%s connected'",
           disp.xrandrName.c_str());
  std::string output = RunCommand(cmd);

  auto bpos = output.find("Brightness:");
  if (bpos != std::string::npos) {
    auto cpos = output.find(':', bpos);
    if (cpos != std::string::npos) {
      std::string val = output.substr(cpos + 1);
      val.erase(0, val.find_first_not_of(" \t"));
      auto nlpos = val.find('\n');
      if (nlpos != std::string::npos) val = val.substr(0, nlpos);
      try {
        return std::stod(val);
      } catch (...) {}
    }
  }

  return 1.0;  // Unknown.
}

// ── Set brightness for a display ───────────────────────────────────

static bool SetDisplayBrightness(const DrmDisplay& disp, double brightness) {
  int value = static_cast<int>(std::clamp(brightness, 0.0, 1.0) * 100);

  // Collect candidate I2C buses to try.
  std::vector<int> buses;
  if (disp.i2cBus >= 0) buses.push_back(disp.i2cBus);
  if (disp.i2cBusDdc >= 0 && disp.i2cBusDdc != disp.i2cBus)
    buses.push_back(disp.i2cBusDdc);

  for (int bus : buses) {
    // Try direct I2C DDC/CI first.
    if (DdcSetBrightness(bus, value)) {
      return true;
    }
    // Try ddcutil as fallback.
    if (DdcutilSetBrightness(bus, value)) {
      return true;
    }
  }

  // Fallback: xrandr software brightness (gamma).
  double clamped = std::clamp(brightness, 0.0, 1.0);
  char brightnessStr[32];
  snprintf(brightnessStr, sizeof(brightnessStr), "%.2f", clamped);

  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("xrandr", "xrandr", "--output", disp.xrandrName.c_str(),
           "--brightness", brightnessStr, nullptr);
    _exit(1);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// ── Cached display list ────────────────────────────────────────────

static std::vector<DrmDisplay> g_drmDisplays;

// ── Software brightness (gamma) via Mutter D-Bus or xrandr ─────────
//
// On GNOME/Wayland: use org.gnome.Mutter.DisplayConfig SetCrtcGamma
// via GDBus.  This is the only way to set per-output gamma on Wayland
// since xrandr --brightness only affects the XWayland virtual display.
//
// On X11: fall back to xrandr --output NAME --brightness FACTOR.
//
// The Mutter D-Bus API:
//   GetResources() -> (serial, crtcs[], outputs[], modes[], ...)
//   GetCrtcGamma(serial, crtc_id) -> (aq red, aq green, aq blue)
//   SetCrtcGamma(serial, crtc_id, aq red, aq green, aq blue)

struct MutterOutputInfo {
  std::string name;   // e.g., "DP-1", "HDMI-1"
  int crtcId;         // Mutter CRTC index (not DRM winsys ID)
  int gammaSize;      // LUT entries (typically 4096)
};

static uint32_t g_mutterSerial = 0;
static std::vector<MutterOutputInfo> g_mutterOutputs;
static bool g_mutterQueried = false;
static bool g_isWayland = false;

// Detect if we're running on native Wayland.
static bool IsWayland() {
  static bool checked = false;
  static bool result = false;
  if (!checked) {
    checked = true;
    const char* wl = getenv("WAYLAND_DISPLAY");
    const char* st = getenv("XDG_SESSION_TYPE");
    result = (wl && wl[0] != '\0') || (st && strcmp(st, "wayland") == 0);
  }
  return result;
}

// Query Mutter's DisplayConfig.GetResources to build output -> CRTC mapping.
// Also fetches gamma LUT size for each active CRTC.
static bool QueryMutterResources() {
  if (g_mutterQueried) return !g_mutterOutputs.empty();
  g_mutterQueried = true;
  g_mutterOutputs.clear();

  g_autoptr(GError) error = nullptr;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) {
    fprintf(stderr, "[BSDisplayControl] D-Bus session bus unavailable: %s\n",
            error ? error->message : "unknown");
    return false;
  }

  // Call GetResources: returns (u serial, a(uxiiiiiuaua{sv}) crtcs,
  //   a(uxiausauaua{sv}) outputs, a(uxuudu) modes, i max_w, i max_h)
  g_autoptr(GVariant) res = g_dbus_connection_call_sync(
      bus, "org.gnome.Shell",
      "/org/gnome/Mutter/DisplayConfig",
      "org.gnome.Mutter.DisplayConfig",
      "GetResources", nullptr, nullptr,
      G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);

  if (!res) {
    fprintf(stderr, "[BSDisplayControl] Mutter GetResources failed: %s\n",
            error ? error->message : "unknown");
    return false;
  }

  // Extract serial (first element).
  g_autoptr(GVariant) vSerial = g_variant_get_child_value(res, 0);
  g_mutterSerial = g_variant_get_uint32(vSerial);

  // Extract outputs array (third element, index 2).
  // Each output: (u id, x winsys_id, i crtc_id, au possible_crtcs,
  //               s name, au modes, au clones, a{sv} properties)
  g_autoptr(GVariant) vOutputs = g_variant_get_child_value(res, 2);
  gsize numOutputs = g_variant_n_children(vOutputs);

  for (gsize i = 0; i < numOutputs; i++) {
    g_autoptr(GVariant) vOut = g_variant_get_child_value(vOutputs, i);

    // crtc_id is at index 2 (int32).
    g_autoptr(GVariant) vCrtcId = g_variant_get_child_value(vOut, 2);
    gint32 crtcId = g_variant_get_int32(vCrtcId);

    // name is at index 4 (string).
    g_autoptr(GVariant) vName = g_variant_get_child_value(vOut, 4);
    const gchar* name = g_variant_get_string(vName, nullptr);

    if (crtcId < 0) continue;  // Output not active.

    MutterOutputInfo info;
    info.name = name;
    info.crtcId = crtcId;
    info.gammaSize = 0;

    // Query gamma LUT size for this CRTC.
    g_autoptr(GError) gammaError = nullptr;
    g_autoptr(GVariant) gammaRes = g_dbus_connection_call_sync(
        bus, "org.gnome.Shell",
        "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig",
        "GetCrtcGamma",
        g_variant_new("(uu)", g_mutterSerial, (guint32)crtcId),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &gammaError);

    if (gammaRes) {
      // Result: (aq red, aq green, aq blue)
      g_autoptr(GVariant) vRed = g_variant_get_child_value(gammaRes, 0);
      info.gammaSize = static_cast<int>(g_variant_n_children(vRed));
    }

    g_mutterOutputs.push_back(info);
  }

  fprintf(stderr, "[BSDisplayControl] Mutter: serial=%u, %zu outputs mapped\n",
          g_mutterSerial, g_mutterOutputs.size());
  for (const auto& o : g_mutterOutputs) {
    fprintf(stderr, "[BSDisplayControl]   %s -> CRTC %d, gamma %d\n",
            o.name.c_str(), o.crtcId, o.gammaSize);
  }

  return !g_mutterOutputs.empty();
}

// Set gamma via Mutter D-Bus SetCrtcGamma.
// factor: 0.0 = black, 1.0 = normal (linear ramp).
static bool SetSoftwareBrightnessWayland(const MutterOutputInfo& output, double factor) {
  double clamped = std::clamp(factor, 0.0, 1.0);
  int size = output.gammaSize;
  if (size <= 0) return false;

  g_autoptr(GError) error = nullptr;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) return false;

  // Build gamma LUT: linear ramp scaled by factor.
  // Each entry: value = (i / (size-1)) * 65535 * factor, as uint16.
  GVariantBuilder redBuilder, greenBuilder, blueBuilder;
  g_variant_builder_init(&redBuilder, G_VARIANT_TYPE("aq"));
  g_variant_builder_init(&greenBuilder, G_VARIANT_TYPE("aq"));
  g_variant_builder_init(&blueBuilder, G_VARIANT_TYPE("aq"));

  double denom = (size > 1) ? static_cast<double>(size - 1) : 1.0;
  for (int i = 0; i < size; i++) {
    guint16 val = static_cast<guint16>(
        std::clamp(static_cast<double>(i) / denom * 65535.0 * clamped, 0.0, 65535.0));
    g_variant_builder_add(&redBuilder, "q", val);
    g_variant_builder_add(&greenBuilder, "q", val);
    g_variant_builder_add(&blueBuilder, "q", val);
  }

  g_autoptr(GVariant) result = g_dbus_connection_call_sync(
      bus, "org.gnome.Shell",
      "/org/gnome/Mutter/DisplayConfig",
      "org.gnome.Mutter.DisplayConfig",
      "SetCrtcGamma",
      g_variant_new("(uu@aq@aq@aq)",
                     g_mutterSerial,
                     static_cast<guint32>(output.crtcId),
                     g_variant_builder_end(&redBuilder),
                     g_variant_builder_end(&greenBuilder),
                     g_variant_builder_end(&blueBuilder)),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);

  if (!result) {
    fprintf(stderr, "[BSDisplayControl] SetCrtcGamma failed for %s: %s\n",
            output.name.c_str(), error ? error->message : "unknown");
    return false;
  }
  return true;
}

// Set gamma via xrandr (X11 fallback).
static bool SetSoftwareBrightnessX11(const std::string& outputName, double factor) {
  double clamped = std::clamp(factor, 0.0, 1.0);
  char gammaStr[32];
  snprintf(gammaStr, sizeof(gammaStr), "%.4f", clamped);

  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("xrandr", "xrandr", "--output", outputName.c_str(),
           "--brightness", gammaStr, nullptr);
    _exit(1);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Find the output name for a given display ID (used for both Wayland and X11).
static std::string FindOutputName(const char* displayId) {
  std::string idStr(displayId);

  if (idStr == "backlight") {
    for (const auto& disp : g_drmDisplays) {
      if (disp.isBuiltIn) return disp.xrandrName;
    }
    for (const auto& name : {"eDP-1", "eDP-2", "LVDS-1"}) {
      return std::string(name);
    }
    return "";
  }

  // DRM display: extract xrandr/output name from cached DrmDisplay.
  for (const auto& disp : g_drmDisplays) {
    if (("drm:" + disp.connector) == idStr) {
      return disp.xrandrName;
    }
  }
  return "";
}

// Set software brightness for a display.
// Dispatches to Wayland (Mutter D-Bus) or X11 (xrandr) based on session type.
static bool SetSoftwareBrightness(const char* displayId, double gamma) {
  std::string outputName = FindOutputName(displayId);
  if (outputName.empty()) return false;

  g_isWayland = IsWayland();

  if (g_isWayland) {
    // Wayland: use Mutter D-Bus.
    if (!g_mutterQueried) QueryMutterResources();

    for (const auto& out : g_mutterOutputs) {
      if (out.name == outputName) {
        return SetSoftwareBrightnessWayland(out, gamma);
      }
    }

    // Output not found — re-query in case monitors changed.
    g_mutterQueried = false;
    QueryMutterResources();
    for (const auto& out : g_mutterOutputs) {
      if (out.name == outputName) {
        return SetSoftwareBrightnessWayland(out, gamma);
      }
    }

    fprintf(stderr, "[BSDisplayControl] Mutter output '%s' not found\n",
            outputName.c_str());
    return false;
  }

  // X11: use xrandr.
  return SetSoftwareBrightnessX11(outputName, gamma);
}

// ── Method channel handler ─────────────────────────────────────────

static void brightness_method_call_handler(FlMethodChannel* channel,
                                           FlMethodCall* method_call,
                                           gpointer user_data) {
  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "getDisplays") == 0) {
    g_autoptr(FlValue) list = fl_value_new_list();

    // 1) Try sysfs backlight (built-in laptop display).
    std::string backlightPath = FindBacklightPath();
    if (!backlightPath.empty()) {
      g_autoptr(FlValue) display = fl_value_new_map();
      fl_value_set_string_take(display, "id", fl_value_new_string("backlight"));

      std::string driverName = std::filesystem::path(backlightPath).filename().string();
      std::string displayName = "Built-in Display (" + driverName + ")";
      fl_value_set_string_take(display, "name", fl_value_new_string(displayName.c_str()));
      fl_value_set_string_take(display, "brightness",
                               fl_value_new_float(GetBacklightBrightness(backlightPath)));
      fl_value_set_string_take(display, "isBuiltIn", fl_value_new_bool(TRUE));
      fl_value_append_take(list, fl_value_ref(display));
    }

    // 2) Enumerate external monitors via DRM sysfs.
    g_drmDisplays = EnumerateDrmDisplays();
    for (const auto& disp : g_drmDisplays) {
      // Skip built-in displays if we already have a backlight entry.
      if (!backlightPath.empty() && disp.isBuiltIn) continue;

      g_autoptr(FlValue) display = fl_value_new_map();

      std::string id = "drm:" + disp.connector;
      fl_value_set_string_take(display, "id", fl_value_new_string(id.c_str()));

      std::string name = disp.edidName.empty() ? disp.xrandrName : disp.edidName;
      fl_value_set_string_take(display, "name", fl_value_new_string(name.c_str()));

      double brightness = GetDisplayBrightness(disp);
      fl_value_set_string_take(display, "brightness", fl_value_new_float(brightness));
      fl_value_set_string_take(display, "isBuiltIn", fl_value_new_bool(disp.isBuiltIn));
      fl_value_append_take(list, fl_value_ref(display));
    }

    fl_method_call_respond_success(method_call, list, nullptr);

  } else if (strcmp(method, "setBrightness") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
      fl_method_call_respond_error(method_call, "INVALID_ARGS", "Expected map", nullptr, nullptr);
      return;
    }

    FlValue* idVal = fl_value_lookup_string(args, "displayId");
    FlValue* brVal = fl_value_lookup_string(args, "brightness");
    if (!idVal || !brVal) {
      fl_method_call_respond_error(method_call, "INVALID_ARGS",
                                   "Missing displayId or brightness", nullptr, nullptr);
      return;
    }

    const char* displayId = fl_value_get_string(idVal);
    double brightness = fl_value_get_float(brVal);
    bool success = false;

    if (strcmp(displayId, "backlight") == 0) {
      std::string backlightPath = FindBacklightPath();
      if (!backlightPath.empty()) {
        success = SetBacklightBrightness(backlightPath, brightness);
      }
    } else {
      // Find the matching DRM display from cached list.
      std::string idStr(displayId);
      bool found = false;
      for (const auto& disp : g_drmDisplays) {
        if (("drm:" + disp.connector) == idStr) {
          success = SetDisplayBrightness(disp, brightness);
          found = true;
          break;
        }
      }
      if (!found) {
        // Display not found in cached list — likely stale data.
      }
    }

    g_autoptr(FlValue) result = fl_value_new_bool(success);
    fl_method_call_respond_success(method_call, result, nullptr);

  } else if (strcmp(method, "setSoftwareBrightness") == 0) {
    FlValue* args = fl_method_call_get_args(method_call);
    if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
      fl_method_call_respond_error(method_call, "INVALID_ARGS", "Expected map", nullptr, nullptr);
      return;
    }

    FlValue* idVal = fl_value_lookup_string(args, "displayId");
    FlValue* gammaVal = fl_value_lookup_string(args, "gamma");
    if (!idVal || !gammaVal) {
      fl_method_call_respond_error(method_call, "INVALID_ARGS",
                                   "Missing displayId or gamma", nullptr, nullptr);
      return;
    }

    const char* displayId = fl_value_get_string(idVal);
    double gamma = fl_value_get_float(gammaVal);

    bool success = SetSoftwareBrightness(displayId, gamma);

    g_autoptr(FlValue) result = fl_value_new_bool(success);
    fl_method_call_respond_success(method_call, result, nullptr);

  } else {
    fl_method_call_respond_not_implemented(method_call, nullptr);
  }
}

// ── Application implementation ─────────────────────────────────────

struct _MyApplication {
  GtkApplication parent_instance;
  char** dart_entrypoint_arguments;
};

G_DEFINE_TYPE(MyApplication, my_application, GTK_TYPE_APPLICATION)

static void first_frame_cb(MyApplication* self, FlView* view) {
  gtk_widget_show(gtk_widget_get_toplevel(GTK_WIDGET(view)));
}

static void my_application_activate(GApplication* application) {
  MyApplication* self = MY_APPLICATION(application);
  GtkWindow* window =
      GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(application)));

  gboolean use_header_bar = TRUE;
#ifdef GDK_WINDOWING_X11
  GdkScreen* screen = gtk_window_get_screen(window);
  if (GDK_IS_X11_SCREEN(screen)) {
    const gchar* wm_name = gdk_x11_screen_get_window_manager_name(screen);
    if (g_strcmp0(wm_name, "GNOME Shell") != 0) {
      use_header_bar = FALSE;
    }
  }
#endif
  if (use_header_bar) {
    GtkHeaderBar* header_bar = GTK_HEADER_BAR(gtk_header_bar_new());
    gtk_widget_show(GTK_WIDGET(header_bar));
    gtk_header_bar_set_title(header_bar, "BS Display Control");
    gtk_header_bar_set_show_close_button(header_bar, TRUE);
    gtk_window_set_titlebar(window, GTK_WIDGET(header_bar));
  } else {
    gtk_window_set_title(window, "BS Display Control");
  }

  gtk_window_set_default_size(window, 800, 600);

  g_autoptr(FlDartProject) project = fl_dart_project_new();
  fl_dart_project_set_dart_entrypoint_arguments(
      project, self->dart_entrypoint_arguments);

  FlView* view = fl_view_new(project);
  GdkRGBA background_color;
  gdk_rgba_parse(&background_color, "#000000");
  fl_view_set_background_color(view, &background_color);
  gtk_widget_show(GTK_WIDGET(view));
  gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));

  // Register the brightness method channel.
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  FlMethodChannel* brightness_channel = fl_method_channel_new(
      fl_engine_get_binary_messenger(fl_view_get_engine(view)),
      "com.bsdisplaycontrol/brightness",
      FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(brightness_channel,
                                            brightness_method_call_handler,
                                            nullptr, nullptr);

  g_signal_connect_swapped(view, "first-frame", G_CALLBACK(first_frame_cb),
                           self);
  gtk_widget_realize(GTK_WIDGET(view));

  fl_register_plugins(FL_PLUGIN_REGISTRY(view));

  gtk_widget_grab_focus(GTK_WIDGET(view));
}

static gboolean my_application_local_command_line(GApplication* application,
                                                  gchar*** arguments,
                                                  int* exit_status) {
  MyApplication* self = MY_APPLICATION(application);
  self->dart_entrypoint_arguments = g_strdupv(*arguments + 1);

  g_autoptr(GError) error = nullptr;
  if (!g_application_register(application, nullptr, &error)) {
    g_warning("Failed to register: %s", error->message);
    *exit_status = 1;
    return TRUE;
  }

  g_application_activate(application);
  *exit_status = 0;

  return TRUE;
}

static void my_application_startup(GApplication* application) {
  G_APPLICATION_CLASS(my_application_parent_class)->startup(application);
}

static void my_application_shutdown(GApplication* application) {
  G_APPLICATION_CLASS(my_application_parent_class)->shutdown(application);
}

static void my_application_dispose(GObject* object) {
  MyApplication* self = MY_APPLICATION(object);
  g_clear_pointer(&self->dart_entrypoint_arguments, g_strfreev);
  G_OBJECT_CLASS(my_application_parent_class)->dispose(object);
}

static void my_application_class_init(MyApplicationClass* klass) {
  G_APPLICATION_CLASS(klass)->activate = my_application_activate;
  G_APPLICATION_CLASS(klass)->local_command_line =
      my_application_local_command_line;
  G_APPLICATION_CLASS(klass)->startup = my_application_startup;
  G_APPLICATION_CLASS(klass)->shutdown = my_application_shutdown;
  G_OBJECT_CLASS(klass)->dispose = my_application_dispose;
}

static void my_application_init(MyApplication* self) {}

MyApplication* my_application_new() {
  g_set_prgname(APPLICATION_ID);

  return MY_APPLICATION(g_object_new(my_application_get_type(),
                                     "application-id", APPLICATION_ID, "flags",
                                     G_APPLICATION_NON_UNIQUE, nullptr));
}

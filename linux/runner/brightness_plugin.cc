#include "brightness_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gio/gio.h>

#include <dirent.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define BRIGHTNESS_CHANNEL "com.bsdisplaycontrol/brightness"

static FlMethodChannel* g_channel = nullptr;

struct Display {
  std::string id;
  std::string name;
  double brightness;
  bool is_built_in;
};

static std::vector<Display> get_backlight_displays() {
  std::vector<Display> displays;
  const char* base = "/sys/class/backlight";
  DIR* dir = opendir(base);
  if (!dir) return displays;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_name[0] == '.') continue;
    std::string name(entry->d_name);
    std::string path = std::string(base) + "/" + name;
    std::ifstream cur(path + "/brightness");
    std::ifstream max(path + "/max_brightness");
    if (!cur || !max) continue;
    int cur_val = 0, max_val = 1;
    cur >> cur_val;
    max >> max_val;
    if (max_val <= 0) continue;
    displays.push_back({
        "backlight:" + name,
        name,
        static_cast<double>(cur_val) / max_val,
        true,
    });
  }
  closedir(dir);
  return displays;
}

static std::string run_cmd(const std::string& cmd) {
  std::string result;
  std::array<char, 256> buf;
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return result;
  while (fgets(buf.data(), buf.size(), pipe)) result += buf.data();
  pclose(pipe);
  return result;
}

// Returns the path to ddcutil: prefers the snap-bundled copy if $SNAP is set,
// otherwise falls back to the system PATH.
static std::string ddcutil_cmd() {
  const char* snap = getenv("SNAP");
  if (snap) {
    std::string path = std::string(snap) + "/usr/bin/ddcutil";
    std::ifstream f(path);
    if (f.good()) return path;
  }
  return "ddcutil";
}

static std::vector<Display> get_ddc_displays() {
  std::vector<Display> displays;
  std::string ddc = ddcutil_cmd();
  std::string out = run_cmd(ddc + " detect --brief 2>/dev/null");
  if (out.empty()) return displays;

  std::istringstream ss(out);
  std::string line;
  std::vector<int> nums;
  while (std::getline(ss, line)) {
    if (line.rfind("Display ", 0) == 0) {
      try {
        nums.push_back(std::stoi(line.substr(8)));
      } catch (...) {
      }
    }
  }

  for (int n : nums) {
    std::string vcp = run_cmd(ddc + " getvcp 10 --display " +
                              std::to_string(n) + " --brief 2>/dev/null");
    double brightness = 0.5;
    std::istringstream vs(vcp);
    std::string vline;
    while (std::getline(vs, vline)) {
      if (vline.find("VCP 10") != std::string::npos ||
          vline.find("VCP 0A") != std::string::npos) {
        std::istringstream ts(vline);
        std::string tok;
        std::vector<std::string> toks;
        while (ts >> tok) toks.push_back(tok);
        if (toks.size() >= 5) {
          try {
            int cur = std::stoi(toks[3]), max = std::stoi(toks[4]);
            if (max > 0) brightness = static_cast<double>(cur) / max;
          } catch (...) {
          }
        }
      }
    }

    std::string name_out =
        run_cmd(ddc + " query --display " + std::to_string(n) +
                " 2>/dev/null | grep -m1 'Monitor name'");
    std::string name = "Display " + std::to_string(n);
    if (!name_out.empty()) {
      size_t c = name_out.find(':');
      if (c != std::string::npos) {
        name = name_out.substr(c + 1);
        name.erase(0, name.find_first_not_of(" \t\r\n"));
        name.erase(name.find_last_not_of(" \t\r\n") + 1);
      }
    }

    displays.push_back({"ddc:" + std::to_string(n), name, brightness, false});
  }
  return displays;
}

// Returns true if the current session is Wayland. xrandr --brightness only
// affects XWayland's emulated gamma and has no visual effect on Wayland
// compositors, so we skip the xrandr fallback entirely in that case.
static bool is_wayland() {
  const char* wd = getenv("WAYLAND_DISPLAY");
  return wd != nullptr && wd[0] != '\0';
}

// Parses `xrandr --verbose` to find connected outputs and their software
// brightness. Only used as a fallback on X11 (not Wayland).
static std::vector<Display> get_xrandr_displays() {
  std::vector<Display> displays;
  if (is_wayland()) return displays;

  std::string out = run_cmd("xrandr --verbose 2>/dev/null");
  if (out.empty()) return displays;

  std::istringstream ss(out);
  std::string line;
  std::string current_output;
  bool connected = false;

  while (std::getline(ss, line)) {
    if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
      current_output.clear();
      connected = false;
      std::istringstream ls(line);
      std::string name, state;
      if (ls >> name >> state) {
        if (state == "connected") {
          current_output = name;
          connected = true;
        }
      }
    } else if (connected && !current_output.empty()) {
      std::string trimmed = line;
      trimmed.erase(0, trimmed.find_first_not_of(" \t"));
      if (trimmed.rfind("Brightness:", 0) == 0) {
        double br = 1.0;
        try {
          br = std::stod(trimmed.substr(11));
        } catch (...) {
        }
        if (br <= 0.0) br = 1.0;
        displays.push_back(
            {"xrandr:" + current_output, current_output, br, false});
        current_output.clear();
        connected = false;
      }
    }
  }

  return displays;
}

static FlValue* make_display_list(const std::vector<Display>& displays) {
  FlValue* list = fl_value_new_list();
  for (const auto& d : displays) {
    FlValue* m = fl_value_new_map();
    fl_value_set_string_take(m, "id", fl_value_new_string(d.id.c_str()));
    fl_value_set_string_take(m, "name", fl_value_new_string(d.name.c_str()));
    fl_value_set_string_take(m, "brightness", fl_value_new_float(d.brightness));
    fl_value_set_string_take(m, "isBuiltIn", fl_value_new_bool(d.is_built_in));
    fl_value_append_take(list, m);
  }
  return list;
}

static void handle_method_call(FlMethodChannel*, FlMethodCall* call,
                               gpointer) {
  const gchar* method = fl_method_call_get_name(call);
  g_autoptr(GError) err = nullptr;

  if (strcmp(method, "getDisplays") == 0) {
    auto displays = get_backlight_displays();
    auto ddc = get_ddc_displays();
    displays.insert(displays.end(), ddc.begin(), ddc.end());

    // xrandr software brightness is a fallback for X11 only; it has no visual
    // effect under Wayland compositors (including GNOME/Mutter with XWayland).
    if (ddc.empty()) {
      auto xrandr = get_xrandr_displays();
      displays.insert(displays.end(), xrandr.begin(), xrandr.end());
    }

    // If nothing was found, return an error so the UI shows a meaningful
    // message rather than "0 displays detected".
    if (displays.empty()) {
      fl_method_call_respond_error(
          call, "NO_DISPLAYS",
          is_wayland()
              ? "No displays found. Install ddcutil and ensure you are in the "
                "i2c group: sudo apt install ddcutil && sudo usermod -aG i2c "
                "$USER (then log out and back in)"
              : "No displays found. Install ddcutil for DDC/CI support: "
                "sudo apt install ddcutil",
          nullptr, &err);
      return;
    }

    g_autoptr(FlValue) result = make_display_list(displays);
    fl_method_call_respond_success(call, result, &err);

  } else if (strcmp(method, "setBrightness") == 0) {
    FlValue* args = fl_method_call_get_args(call);
    FlValue* id_v = fl_value_lookup_string(args, "displayId");
    FlValue* br_v = fl_value_lookup_string(args, "brightness");
    if (!id_v || !br_v) {
      fl_method_call_respond_error(call, "INVALID_ARGS",
                                   "Missing displayId or brightness", nullptr,
                                   &err);
      return;
    }

    std::string id(fl_value_get_string(id_v));
    double br = std::min(1.0, std::max(0.0, fl_value_get_float(br_v)));
    bool ok = false;

    if (id.rfind("backlight:", 0) == 0) {
      std::string name = id.substr(10);
      std::ifstream mf("/sys/class/backlight/" + name + "/max_brightness");
      if (mf) {
        int max_v = 1;
        mf >> max_v;
        guint32 target = static_cast<guint32>(br * max_v + 0.5);

        GError* dbus_err = nullptr;
        GDBusConnection* conn =
            g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &dbus_err);
        if (conn) {
          GVariant* res = g_dbus_connection_call_sync(
              conn, "org.freedesktop.login1",
              "/org/freedesktop/login1/session/auto",
              "org.freedesktop.login1.Session", "SetBrightness",
              g_variant_new("(ssu)", "backlight", name.c_str(), target),
              nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &dbus_err);
          ok = (res != nullptr);
          if (res) g_variant_unref(res);
          g_object_unref(conn);
        }
        if (dbus_err) g_error_free(dbus_err);

        if (!ok) {
          std::ofstream of("/sys/class/backlight/" + name + "/brightness");
          if (of) {
            of << static_cast<int>(target);
            ok = of.good();
          }
        }
      }
    } else if (id.rfind("ddc:", 0) == 0) {
      std::string n = id.substr(4);
      int pct = static_cast<int>(br * 100 + 0.5);
      std::string cmd = ddcutil_cmd() + " setvcp 10 " + std::to_string(pct) +
                        " --display " + n + " 2>/dev/null";
      // NOLINTNEXTLINE(cert-env33-c)
      ok = (system(cmd.c_str()) == 0);
    } else if (id.rfind("xrandr:", 0) == 0) {
      std::string output = id.substr(7);
      double clamped = std::max(0.05, br);
      std::ostringstream cmd;
      cmd << "xrandr --output " << output << " --brightness " << clamped
          << " 2>/dev/null";
      // NOLINTNEXTLINE(cert-env33-c)
      ok = (system(cmd.str().c_str()) == 0);
    }

    g_autoptr(FlValue) result = fl_value_new_bool(ok);
    fl_method_call_respond_success(call, result, &err);

  } else {
    fl_method_call_respond_not_implemented(call, &err);
  }
}

void brightness_plugin_register(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "BrightnessPlugin");
  FlBinaryMessenger* messenger = fl_plugin_registrar_get_messenger(registrar);
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_channel = fl_method_channel_new(messenger, BRIGHTNESS_CHANNEL,
                                    FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(g_channel, handle_method_call,
                                            nullptr, nullptr);
}

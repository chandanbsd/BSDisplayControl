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
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include "flutter/generated_plugin_registrant.h"

// ── Display info structure ─────────────────────────────────────────

struct DisplayInfo {
  std::string id;
  std::string name;
  double brightness;
  bool isBuiltIn;
};

// ── Brightness control via sysfs (backlight) ───────────────────────

static std::string FindBacklightPath() {
  const std::string basePath = "/sys/class/backlight";
  if (!std::filesystem::exists(basePath)) return "";

  // Prefer intel_backlight, then acpi_video0, then any other.
  std::vector<std::string> preferred = {"intel_backlight", "amdgpu_bl0", "amdgpu_bl1", "acpi_video0"};
  for (const auto& name : preferred) {
    auto path = basePath + "/" + name;
    if (std::filesystem::exists(path)) return path;
  }

  // Fall back to first available.
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
  // Ensure at least 1 to avoid turning off completely.
  if (newValue < 1 && brightness > 0.0) newValue = 1;

  std::ofstream curFile(backlightPath + "/brightness");
  if (curFile.is_open()) {
    curFile << newValue;
    return curFile.good();
  }

  // Fallback: use fork/exec with tee for permission issues (avoids system()).
  std::string brightnessFile = backlightPath + "/brightness";
  std::string valueStr = std::to_string(newValue);

  // Create a pipe: write the value into it, then exec tee to write to the file.
  int pipefd[2];
  if (pipe(pipefd) != 0) return false;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }

  if (pid == 0) {
    // Child: redirect stdin from pipe, redirect stdout to /dev/null.
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

  // Parent: write value into pipe, then wait for child.
  close(pipefd[0]);
  write(pipefd[1], valueStr.c_str(), valueStr.size());
  close(pipefd[1]);

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// ── Brightness control via xrandr (external monitors) ──────────────

struct XrandrOutput {
  std::string name;
  bool connected;
  double brightness;
};

static std::vector<XrandrOutput> GetXrandrOutputs() {
  std::vector<XrandrOutput> outputs;

  FILE* pipe = popen("xrandr --verbose 2>/dev/null", "r");
  if (!pipe) return outputs;

  char line[1024];
  XrandrOutput current;
  bool inOutput = false;

  while (fgets(line, sizeof(line), pipe)) {
    std::string l(line);

    // Detect output lines like "HDMI-1 connected" or "eDP-1 connected".
    if (l.find(" connected") != std::string::npos || l.find(" disconnected") != std::string::npos) {
      if (inOutput && current.connected) {
        outputs.push_back(current);
      }
      current = {};
      // Extract output name (first word).
      size_t spacePos = l.find(' ');
      if (spacePos != std::string::npos) {
        current.name = l.substr(0, spacePos);
      }
      current.connected = l.find(" connected") != std::string::npos;
      current.brightness = 1.0;
      inOutput = true;
    }
    // Look for Brightness property.
    else if (inOutput && l.find("Brightness:") != std::string::npos) {
      size_t colonPos = l.find(':');
      if (colonPos != std::string::npos) {
        std::string val = l.substr(colonPos + 1);
        // Trim whitespace.
        val.erase(0, val.find_first_not_of(" \t\n\r"));
        val.erase(val.find_last_not_of(" \t\n\r") + 1);
        try {
          current.brightness = std::stod(val);
        } catch (...) {
          current.brightness = 1.0;
        }
      }
    }
  }

  // Don't forget the last output.
  if (inOutput && current.connected) {
    outputs.push_back(current);
  }

  pclose(pipe);
  return outputs;
}

static bool SetXrandrBrightness(const std::string& outputName, double brightness) {
  double clamped = std::clamp(brightness, 0.0, 1.0);
  // xrandr --output <name> --brightness <value>
  // Note: xrandr brightness is a software gamma, not hardware DDC/CI.

  char brightnessStr[32];
  snprintf(brightnessStr, sizeof(brightnessStr), "%.2f", clamped);

  pid_t pid = fork();
  if (pid < 0) return false;

  if (pid == 0) {
    // Child: redirect stdout/stderr to /dev/null.
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    execlp("xrandr", "xrandr", "--output", outputName.c_str(),
           "--brightness", brightnessStr, nullptr);
    _exit(1);
  }

  // Parent: wait for child.
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// ── Method channel handler ─────────────────────────────────────────

static void brightness_method_call_handler(FlMethodChannel* channel,
                                           FlMethodCall* method_call,
                                           gpointer user_data) {
  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "getDisplays") == 0) {
    g_autoptr(FlValue) list = fl_value_new_list();

    // 1) Try sysfs backlight (built-in display).
    std::string backlightPath = FindBacklightPath();
    if (!backlightPath.empty()) {
      g_autoptr(FlValue) display = fl_value_new_map();
      fl_value_set_string_take(display, "id", fl_value_new_string("backlight"));

      // Extract the backlight driver name for display name.
      std::string driverName = std::filesystem::path(backlightPath).filename().string();
      std::string displayName = "Built-in Display (" + driverName + ")";
      fl_value_set_string_take(display, "name", fl_value_new_string(displayName.c_str()));
      fl_value_set_string_take(display, "brightness",
                               fl_value_new_float(GetBacklightBrightness(backlightPath)));
      fl_value_set_string_take(display, "isBuiltIn", fl_value_new_bool(TRUE));
      fl_value_append_take(list, fl_value_ref(display));
    }

    // 2) Enumerate xrandr outputs for external monitors.
    auto xrandrOutputs = GetXrandrOutputs();
    for (const auto& output : xrandrOutputs) {
      // Skip eDP (embedded display) if we already have a backlight entry.
      if (!backlightPath.empty() &&
          (output.name.find("eDP") == 0 || output.name.find("LVDS") == 0)) {
        continue;
      }

      g_autoptr(FlValue) display = fl_value_new_map();
      fl_value_set_string_take(display, "id", fl_value_new_string(output.name.c_str()));
      fl_value_set_string_take(display, "name", fl_value_new_string(output.name.c_str()));
      fl_value_set_string_take(display, "brightness", fl_value_new_float(output.brightness));
      bool isBuiltIn = output.name.find("eDP") == 0 || output.name.find("LVDS") == 0;
      fl_value_set_string_take(display, "isBuiltIn", fl_value_new_bool(isBuiltIn));
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
      // Treat as xrandr output name.
      success = SetXrandrBrightness(displayId, brightness);
    }

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

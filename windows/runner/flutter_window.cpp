#include "flutter_window.h"

#include <optional>
#include <string>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <wbemidl.h>
#include <comdef.h>

#pragma comment(lib, "Dxva2.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include "flutter/generated_plugin_registrant.h"

// ── Helper structures ──────────────────────────────────────────────

struct MonitorInfo {
  std::string id;
  std::string name;
  double brightness;
  bool isBuiltIn;
};

// ── Monitor enumeration callback ───────────────────────────────────

struct EnumContext {
  std::vector<MonitorInfo> monitors;
  int index;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) {
  auto* ctx = reinterpret_cast<EnumContext*>(dwData);

  MONITORINFOEXW monInfo{};
  monInfo.cbSize = sizeof(MONITORINFOEXW);
  GetMonitorInfoW(hMonitor, &monInfo);

  // Get physical monitors for brightness control.
  DWORD physicalCount = 0;
  if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &physicalCount) || physicalCount == 0) {
    // Fallback: might be a built-in display without DDC/CI support.
    MonitorInfo info;
    info.id = std::to_string(ctx->index);
    // Convert wide device name to narrow string.
    char deviceName[64]{};
    WideCharToMultiByte(CP_UTF8, 0, monInfo.szDevice, -1, deviceName, sizeof(deviceName), nullptr, nullptr);
    info.name = std::string("Display ") + std::to_string(ctx->index + 1) + " (" + deviceName + ")";
    info.isBuiltIn = (monInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;

    // Try WMI to get brightness for built-in laptop displays.
    DWORD wmiBrightness = 0;
    if (info.isBuiltIn && GetWmiBrightness(wmiBrightness)) {
      info.brightness = static_cast<double>(wmiBrightness) / 100.0;
    } else {
      info.brightness = 1.0;
    }

    ctx->monitors.push_back(info);
    ctx->index++;
    return TRUE;
  }

  std::vector<PHYSICAL_MONITOR> physicalMonitors(physicalCount);
  if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, physicalCount, physicalMonitors.data())) {
    ctx->index++;
    return TRUE;
  }

  for (DWORD i = 0; i < physicalCount; ++i) {
    MonitorInfo info;
    info.id = std::to_string(ctx->index);

    // Convert wide description to narrow string.
    char desc[256]{};
    WideCharToMultiByte(CP_UTF8, 0, physicalMonitors[i].szPhysicalMonitorDescription, -1,
                        desc, sizeof(desc), nullptr, nullptr);
    info.name = (strlen(desc) > 0) ? std::string(desc) : ("Display " + std::to_string(ctx->index + 1));

    // Try to get brightness via DDC/CI.
    DWORD minBrightness = 0, curBrightness = 0, maxBrightness = 100;
    if (GetMonitorBrightness(physicalMonitors[i].hPhysicalMonitor,
                             &minBrightness, &curBrightness, &maxBrightness) && maxBrightness > minBrightness) {
      info.brightness = static_cast<double>(curBrightness - minBrightness) /
                        static_cast<double>(maxBrightness - minBrightness);
    } else {
      info.brightness = 1.0;
    }

    info.isBuiltIn = (monInfo.dwFlags & MONITORINFOF_PRIMARY) != 0 && physicalCount == 1;
    ctx->monitors.push_back(info);
    ctx->index++;
  }

  DestroyPhysicalMonitors(physicalCount, physicalMonitors.data());
  return TRUE;
}

// ── WMI brightness control for built-in laptop displays ────────────

static bool GetWmiBrightness(DWORD& outBrightness) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool needUninit = SUCCEEDED(hr);
  // S_FALSE means already initialized — that's fine.
  if (FAILED(hr) && hr != S_FALSE) return false;

  bool result = false;
  IWbemLocator* pLocator = nullptr;
  IWbemServices* pServices = nullptr;
  IEnumWbemClassObject* pEnumerator = nullptr;

  hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IWbemLocator, reinterpret_cast<void**>(&pLocator));
  if (FAILED(hr)) goto cleanup;

  hr = pLocator->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
                               0, nullptr, nullptr, &pServices);
  if (FAILED(hr)) goto cleanup;

  hr = CoSetProxyBlanket(pServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                         nullptr, RPC_C_AUTHN_LEVEL_CALL,
                         RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
  if (FAILED(hr)) goto cleanup;

  hr = pServices->ExecQuery(
      _bstr_t(L"WQL"),
      _bstr_t(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE"),
      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator);
  if (FAILED(hr)) goto cleanup;

  {
    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;
    hr = pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &returned);
    if (SUCCEEDED(hr) && returned > 0) {
      VARIANT vtBrightness;
      hr = pObj->Get(L"CurrentBrightness", 0, &vtBrightness, nullptr, nullptr);
      if (SUCCEEDED(hr)) {
        outBrightness = vtBrightness.intVal;
        result = true;
        VariantClear(&vtBrightness);
      }
      pObj->Release();
    }
  }

cleanup:
  if (pEnumerator) pEnumerator->Release();
  if (pServices) pServices->Release();
  if (pLocator) pLocator->Release();
  if (needUninit) CoUninitialize();
  return result;
}

static bool SetWmiBrightness(DWORD brightness) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool needUninit = SUCCEEDED(hr);
  if (FAILED(hr) && hr != S_FALSE) return false;

  bool result = false;
  IWbemLocator* pLocator = nullptr;
  IWbemServices* pServices = nullptr;
  IEnumWbemClassObject* pEnumerator = nullptr;

  hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IWbemLocator, reinterpret_cast<void**>(&pLocator));
  if (FAILED(hr)) goto cleanup;

  hr = pLocator->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
                               0, nullptr, nullptr, &pServices);
  if (FAILED(hr)) goto cleanup;

  hr = CoSetProxyBlanket(pServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                         nullptr, RPC_C_AUTHN_LEVEL_CALL,
                         RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
  if (FAILED(hr)) goto cleanup;

  hr = pServices->ExecQuery(
      _bstr_t(L"WQL"),
      _bstr_t(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE"),
      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator);
  if (FAILED(hr)) goto cleanup;

  {
    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;
    hr = pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &returned);
    if (SUCCEEDED(hr) && returned > 0) {
      // Get the class and input parameters for WmiSetBrightness.
      IWbemClassObject* pClass = nullptr;
      hr = pServices->GetObject(_bstr_t(L"WmiMonitorBrightnessMethods"),
                                0, nullptr, &pClass, nullptr);
      if (SUCCEEDED(hr)) {
        IWbemClassObject* pInParamsDefinition = nullptr;
        hr = pClass->GetMethod(L"WmiSetBrightness", 0, &pInParamsDefinition, nullptr);
        if (SUCCEEDED(hr)) {
          IWbemClassObject* pInParams = nullptr;
          hr = pInParamsDefinition->SpawnInstance(0, &pInParams);
          if (SUCCEEDED(hr)) {
            VARIANT vtTimeout;
            VariantInit(&vtTimeout);
            vtTimeout.vt = VT_I4;
            vtTimeout.intVal = 0;
            pInParams->Put(L"Timeout", 0, &vtTimeout, 0);

            VARIANT vtBrightness;
            VariantInit(&vtBrightness);
            vtBrightness.vt = VT_UI1;
            vtBrightness.bVal = static_cast<BYTE>(std::clamp(brightness, (DWORD)0, (DWORD)100));
            pInParams->Put(L"Brightness", 0, &vtBrightness, 0);

            // Get the object path for the instance.
            VARIANT vtPath;
            hr = pObj->Get(L"__PATH", 0, &vtPath, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
              IWbemClassObject* pOutParams = nullptr;
              hr = pServices->ExecMethod(vtPath.bstrVal, _bstr_t(L"WmiSetBrightness"),
                                         0, nullptr, pInParams, &pOutParams, nullptr);
              result = SUCCEEDED(hr);
              if (pOutParams) pOutParams->Release();
              VariantClear(&vtPath);
            }
            pInParams->Release();
          }
          pInParamsDefinition->Release();
        }
        pClass->Release();
      }
      pObj->Release();
    }
  }

cleanup:
  if (pEnumerator) pEnumerator->Release();
  if (pServices) pServices->Release();
  if (pLocator) pLocator->Release();
  if (needUninit) CoUninitialize();
  return result;
}

// ── Software brightness via gamma ramp ─────────────────────────────
//
// SetDeviceGammaRamp scales the display's color lookup table.
// A gamma factor of 1.0 = normal, 0.0 = fully black.

static bool SetSoftwareBrightnessById(const std::string& displayId, double gamma) {
  int targetIndex;
  try {
    targetIndex = std::stoi(displayId);
  } catch (const std::exception&) {
    return false;
  }

  double clamped = std::clamp(gamma, 0.0, 1.0);

  struct GammaCtx {
    int targetIndex;
    int currentIndex;
    double gamma;
    bool success;
  };

  GammaCtx gammaCtx{targetIndex, 0, clamped, false};

  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) -> BOOL {
    auto* ctx = reinterpret_cast<GammaCtx*>(dwData);

    if (ctx->currentIndex == ctx->targetIndex) {
      // Get the device context for this monitor.
      MONITORINFOEXW monInfo{};
      monInfo.cbSize = sizeof(MONITORINFOEXW);
      GetMonitorInfoW(hMonitor, &monInfo);

      HDC hdc = CreateDCW(monInfo.szDevice, monInfo.szDevice, nullptr, nullptr);
      if (!hdc) return FALSE;

      // Build a gamma ramp scaled by the gamma factor.
      WORD gammaRamp[3][256];
      for (int i = 0; i < 256; ++i) {
        WORD value = static_cast<WORD>(i * 256 * ctx->gamma);
        gammaRamp[0][i] = value;  // Red
        gammaRamp[1][i] = value;  // Green
        gammaRamp[2][i] = value;  // Blue
      }

      ctx->success = SetDeviceGammaRamp(hdc, gammaRamp) != 0;
      DeleteDC(hdc);
      return FALSE;  // Stop enumerating.
    }

    ctx->currentIndex++;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&gammaCtx));

  return gammaCtx.success;
}

// ── Set brightness for a specific monitor ──────────────────────────

static bool SetMonitorBrightnessById(const std::string& displayId, double brightness) {
  int targetIndex;
  try {
    targetIndex = std::stoi(displayId);
  } catch (const std::exception&) {
    return false;
  }

  struct SetCtx {
    int targetIndex;
    int currentIndex;
    double brightness;
    bool success;
  };

  SetCtx setCtx{targetIndex, 0, brightness, false};

  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) -> BOOL {
    auto* ctx = reinterpret_cast<SetCtx*>(dwData);

    DWORD physicalCount = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &physicalCount) || physicalCount == 0) {
      if (ctx->currentIndex == ctx->targetIndex) {
        // No DDC/CI support — try WMI for built-in laptop displays.
        DWORD wmiBrightness = static_cast<DWORD>(std::clamp(ctx->brightness, 0.0, 1.0) * 100.0);
        ctx->success = SetWmiBrightness(wmiBrightness);
        return FALSE;
      }
      ctx->currentIndex++;
      return TRUE;
    }

    std::vector<PHYSICAL_MONITOR> physicalMonitors(physicalCount);
    if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, physicalCount, physicalMonitors.data())) {
      ctx->currentIndex++;
      return TRUE;
    }

    for (DWORD i = 0; i < physicalCount; ++i) {
      if (ctx->currentIndex == ctx->targetIndex) {
        DWORD minB = 0, curB = 0, maxB = 100;
        GetMonitorBrightness(physicalMonitors[i].hPhysicalMonitor, &minB, &curB, &maxB);

        DWORD newBrightness = static_cast<DWORD>(
            minB + (maxB - minB) * std::clamp(ctx->brightness, 0.0, 1.0));
        ctx->success = SetMonitorBrightness(physicalMonitors[i].hPhysicalMonitor, newBrightness) != 0;

        DestroyPhysicalMonitors(physicalCount, physicalMonitors.data());
        return FALSE; // Stop enumerating.
      }
      ctx->currentIndex++;
    }

    DestroyPhysicalMonitors(physicalCount, physicalMonitors.data());
    return TRUE;
  }, reinterpret_cast<LPARAM>(&setCtx));

  return setCtx.success;
}

// ── FlutterWindow implementation ───────────────────────────────────

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  // Set up the brightness method channel.
  SetUpBrightnessChannel();

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::SetUpBrightnessChannel() {
  auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(),
      "com.chandanbsd.bsdisplaycontrol/brightness",
      &flutter::StandardMethodCodec::GetInstance());

  channel->SetMethodCallHandler(
      [](const flutter::MethodCall<flutter::EncodableValue>& call,
         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

        if (call.method_name() == "getDisplays") {
          EnumContext ctx{{}, 0};
          EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));

          flutter::EncodableList displayList;
          for (const auto& mon : ctx.monitors) {
            flutter::EncodableMap map;
            map[flutter::EncodableValue("id")] = flutter::EncodableValue(mon.id);
            map[flutter::EncodableValue("name")] = flutter::EncodableValue(mon.name);
            map[flutter::EncodableValue("brightness")] = flutter::EncodableValue(mon.brightness);
            map[flutter::EncodableValue("isBuiltIn")] = flutter::EncodableValue(mon.isBuiltIn);
            displayList.push_back(flutter::EncodableValue(map));
          }
          result->Success(flutter::EncodableValue(displayList));

        } else if (call.method_name() == "setBrightness") {
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("INVALID_ARGS", "Expected a map of arguments");
            return;
          }

          auto idIt = args->find(flutter::EncodableValue("displayId"));
          auto brIt = args->find(flutter::EncodableValue("brightness"));
          if (idIt == args->end() || brIt == args->end()) {
            result->Error("INVALID_ARGS", "Missing displayId or brightness");
            return;
          }

          std::string displayId;
          double brightness;
          try {
            displayId = std::get<std::string>(idIt->second);
            brightness = std::get<double>(brIt->second);
          } catch (const std::bad_variant_access&) {
            result->Error("INVALID_ARGS", "displayId must be a string and brightness must be a double");
            return;
          }

          bool success = SetMonitorBrightnessById(displayId, brightness);
          result->Success(flutter::EncodableValue(success));

        } else if (call.method_name() == "setSoftwareBrightness") {
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          if (!args) {
            result->Error("INVALID_ARGS", "Expected a map of arguments");
            return;
          }

          auto idIt = args->find(flutter::EncodableValue("displayId"));
          auto gammaIt = args->find(flutter::EncodableValue("gamma"));
          if (idIt == args->end() || gammaIt == args->end()) {
            result->Error("INVALID_ARGS", "Missing displayId or gamma");
            return;
          }

          std::string displayId;
          double gamma;
          try {
            displayId = std::get<std::string>(idIt->second);
            gamma = std::get<double>(gammaIt->second);
          } catch (const std::bad_variant_access&) {
            result->Error("INVALID_ARGS", "displayId must be a string and gamma must be a double");
            return;
          }

          bool success = SetSoftwareBrightnessById(displayId, gamma);
          result->Success(flutter::EncodableValue(success));

        } else {
          result->NotImplemented();
        }
      });
}

void FlutterWindow::OnDestroy() {
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

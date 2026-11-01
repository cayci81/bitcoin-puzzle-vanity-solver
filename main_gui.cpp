#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <tchar.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <memory>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "GpuEngine.h"
#include "GuiController.h"
#include "cli.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// DirectX 11 state (from canonical ImGui example_win32_directx11)
ID3D11Device*            g_pd3dDevice           = nullptr;
ID3D11DeviceContext*     g_pd3dDeviceContext    = nullptr;
IDXGISwapChain*          g_pSwapChain           = nullptr;
bool                     g_SwapChainOccluded    = false;
UINT                     g_ResizeWidth          = 0;
UINT                     g_ResizeHeight         = 0;
ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

void CreateRenderTarget() {
  ID3D11Texture2D* pBackBuffer = nullptr;
  if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) &&
      pBackBuffer != nullptr) {
    g_pd3dDevice->CreateRenderTargetView(
        pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
  }
}

void CleanupRenderTarget() {
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
}

bool CreateDeviceD3D(HWND hWnd) {
  DXGI_SWAP_CHAIN_DESC sd;
  ZeroMemory(&sd, sizeof(sd));
  sd.BufferCount                        = 2;
  sd.BufferDesc.Width                   = 0;
  sd.BufferDesc.Height                  = 0;
  sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator   = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow                       = hWnd;
  sd.SampleDesc.Count                   = 1;
  sd.SampleDesc.Quality                 = 0;
  sd.Windowed                           = TRUE;
  sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

  UINT createDeviceFlags = 0;
  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL featureLevelArray[2] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_0,
  };
  HRESULT res = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
      featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
      &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  if (res == DXGI_ERROR_UNSUPPORTED) {
    res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  }
  if (res != S_OK) {
    return false;
  }

  CreateRenderTarget();
  return true;
}

void CleanupDeviceD3D() {
  CleanupRenderTarget();
  if (g_pSwapChain) {
    g_pSwapChain->Release();
    g_pSwapChain = nullptr;
  }
  if (g_pd3dDeviceContext) {
    g_pd3dDeviceContext->Release();
    g_pd3dDeviceContext = nullptr;
  }
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = nullptr;
  }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
    return true;
  }

  switch (msg) {
    case WM_SIZE:
      if (wParam == SIZE_MINIMIZED) {
        return 0;
      }
      g_ResizeWidth  = static_cast<UINT>(LOWORD(lParam));
      g_ResizeHeight = static_cast<UINT>(HIWORD(lParam));
      return 0;
    case WM_SYSCOMMAND:
      if ((wParam & 0xfff0) == SC_KEYMENU) {  // Disable ALT application menu
        return 0;
      }
      break;
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
  }
  return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

using puzzle::RangeKey;
using puzzle::gui::GuiController;
using puzzle::gui::GuiState;
using puzzle::gui::GuiStats;

void FormatCurrentTimeHms(char* buf, size_t buf_size) {
  std::time_t now = std::time(nullptr);
  std::tm local_tm{};
  localtime_s(&local_tm, &now);
  std::snprintf(buf, buf_size, "%02d:%02d:%02d",
                local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
}

struct AppState {
  puzzle::cli::CliOptions options;
  std::unique_ptr<GuiController> controller;
  char address_buf[128]    = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  char range_start_buf[80] = "AB00";
  char range_end_buf[80]   = "ABFF";
  // Default chunk size: 100M when CUDA GPU is available, 10M on CPU-only builds.
  uint64_t chunk_size_u64 = puzzle::GpuEngine::isAvailable() ? 100'000'000ULL : 10'000'000ULL;
  int  gpu_stride_int      = 262144;
  int  engine_idx          = puzzle::GpuEngine::isAvailable() ? 1 : 0;   // 0=cpu, 1=gpu
  char output_dir_buf[512] = "./puzzle71_results";
  char prefix_hex_buf[256] = {0};
  char last_error_buf[256] = {0};   // snapshot of getStats().last_error
  char last_action_buf[128] = "Last action: none";
  GuiState prev_state      = GuiState::Idle;
  // Throttled file existence cache (polled at most 1 Hz or on state/match change)
  std::chrono::steady_clock::time_point last_fs_check{};
  uint64_t last_fs_matches = UINT64_MAX;
  GuiState last_fs_state   = GuiState::Idle;
  bool cached_has_matches  = false;
  bool cached_has_log      = false;
};

void DrawConfigPanel(AppState& app, const GuiStats& stats) {
  ImGui::Text("Configuration");
  ImGui::Separator();

  const bool can_edit = (stats.state == GuiState::Idle ||
                         stats.state == GuiState::Stopped ||
                         stats.state == GuiState::Found ||
                         stats.state == GuiState::Exhausted ||
                         stats.state == GuiState::Error);

  ImGui::BeginDisabled(!can_edit);
  if (ImGui::SmallButton("Load Puzzle 71 range")) {
    std::strncpy(app.address_buf,
                 "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
                 sizeof(app.address_buf) - 1);
    app.address_buf[sizeof(app.address_buf) - 1] = '\0';
    std::strncpy(app.range_start_buf, "400000000000000000",
                 sizeof(app.range_start_buf) - 1);
    app.range_start_buf[sizeof(app.range_start_buf) - 1] = '\0';
    std::strncpy(app.range_end_buf, "7fffffffffffffffff",
                 sizeof(app.range_end_buf) - 1);
    app.range_end_buf[sizeof(app.range_end_buf) - 1] = '\0';
    app.chunk_size_u64 = (app.engine_idx == 1) ? 100'000'000ULL : 10'000'000ULL;
    app.gpu_stride_int = 262144;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Demo range AB00:ABFF")) {
    std::strncpy(app.address_buf,
                 "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
                 sizeof(app.address_buf) - 1);
    app.address_buf[sizeof(app.address_buf) - 1] = '\0';
    std::strncpy(app.range_start_buf, "AB00",
                 sizeof(app.range_start_buf) - 1);
    app.range_start_buf[sizeof(app.range_start_buf) - 1] = '\0';
    std::strncpy(app.range_end_buf, "ABFF",
                 sizeof(app.range_end_buf) - 1);
    app.range_end_buf[sizeof(app.range_end_buf) - 1] = '\0';
    app.chunk_size_u64 = 256ULL;
    app.gpu_stride_int = 256;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Load from config")) {
    app.last_error_buf[0] = '\0';
    try {
      std::filesystem::path cfg_path = "configs/puzzle71.toml";
      if (!std::filesystem::exists(cfg_path) &&
          std::filesystem::exists("../configs/puzzle71.toml")) {
        cfg_path = "../configs/puzzle71.toml";
      }
      puzzle::cli::CliOptions loaded;
      puzzle::cli::LoadConfigFile(cfg_path, loaded);
      std::snprintf(app.address_buf, sizeof(app.address_buf), "%s",
                    loaded.address.c_str());
      std::snprintf(app.range_start_buf, sizeof(app.range_start_buf), "%s",
                    loaded.range_start.ToHex().c_str());
      std::snprintf(app.range_end_buf, sizeof(app.range_end_buf), "%s",
                    loaded.range_end.ToHex().c_str());
      app.chunk_size_u64 = loaded.chunk_size;
      app.gpu_stride_int = static_cast<int>(loaded.gpu_stride);
      std::snprintf(app.output_dir_buf, sizeof(app.output_dir_buf), "%s",
                    loaded.output_dir.string().c_str());
      std::snprintf(app.prefix_hex_buf, sizeof(app.prefix_hex_buf), "%s",
                    loaded.prefix_hex_csv.c_str());
      if (loaded.engine == "gpu" && puzzle::GpuEngine::isAvailable()) {
        app.engine_idx = 1;
      } else {
        app.engine_idx = 0;
      }
      std::snprintf(app.last_action_buf, sizeof(app.last_action_buf),
                    "Loaded %s", cfg_path.string().c_str());
    } catch (const std::exception& e) {
      std::snprintf(app.last_error_buf, sizeof(app.last_error_buf),
                    "Config load failed: %s", e.what());
    }
  }
  ImGui::InputText("Address", app.address_buf, sizeof(app.address_buf));
  ImGui::InputText("Range Start (hex)", app.range_start_buf, sizeof(app.range_start_buf));
  ImGui::InputText("Range End (hex)", app.range_end_buf, sizeof(app.range_end_buf));
  ImGui::InputScalar("Chunk Size", ImGuiDataType_U64, &app.chunk_size_u64);
  ImGui::TextDisabled("(CPU: 10M-50M recommended. GPU: 100M-1B recommended.)");
  ImGui::InputInt("GPU Stride", &app.gpu_stride_int);
  const char* engines[] = {"cpu", "gpu"};
#ifdef PUZZLE71_ENABLE_CUDA
  if (ImGui::Combo("Engine", &app.engine_idx, engines, 2)) {
    if (app.engine_idx == 1 && app.chunk_size_u64 == 10'000'000ULL) {
      app.chunk_size_u64 = 100'000'000ULL;
    } else if (app.engine_idx == 0 && app.chunk_size_u64 == 100'000'000ULL) {
      app.chunk_size_u64 = 10'000'000ULL;
    }
  }
#else
  // gpu option visible but disabled
  if (ImGui::Combo("Engine", &app.engine_idx, engines, 2)) {
    if (app.engine_idx == 1) {
      // revert to cpu if gpu not available
      app.engine_idx = 0;
    }
  }
  if (app.engine_idx == 1) {
    app.engine_idx = 0;
  }
  ImGui::TextWrapped("(GPU backend ships in Phase 4 — CPU only for now)");
#endif
  ImGui::InputText("Output Dir", app.output_dir_buf, sizeof(app.output_dir_buf));
  ImGui::InputText("Prefix Hex (csv)", app.prefix_hex_buf, sizeof(app.prefix_hex_buf));
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::Separator();

  // Start button: enabled only when can_edit AND no controller running
  const bool can_start = can_edit;
  ImGui::BeginDisabled(!can_start);
  if (ImGui::Button("Start", ImVec2(120, 0))) {
    app.last_error_buf[0] = '\0';
    try {
      if (app.chunk_size_u64 == 0 || app.gpu_stride_int <= 0) {
        throw std::invalid_argument("chunk_size and gpu_stride must be > 0");
      }
      // Build CliOptions from UI state
      puzzle::cli::CliOptions opts;
      opts.address        = app.address_buf;
      opts.range_start    = RangeKey::FromHex(app.range_start_buf);
      opts.range_end      = RangeKey::FromHex(app.range_end_buf);
      opts.chunk_size     = app.chunk_size_u64;
      opts.gpu_stride     = static_cast<uint64_t>(app.gpu_stride_int);
      opts.engine         = (app.engine_idx == 0) ? "cpu" : "gpu";
      opts.output_dir     = app.output_dir_buf;
      opts.prefix_hex_csv = app.prefix_hex_buf;

      // When the user runs a small demo range (e.g. AB00:ABFF = 256 keys)
      // while leaving Chunk Size / GPU Stride at the 10M / 262K defaults,
      // clamp chunk_size and gpu_stride down to the range span so
      // RandomChunkGenerator does not reject the range as smaller than
      // effective_chunk_keys.
      if (opts.range_start <= opts.range_end &&
          opts.chunk_size >= opts.gpu_stride) {
        Int span = opts.range_start.distanceTo(opts.range_end);
        span.AddOne();
        if (span.bits64[1] == 0 && span.bits64[2] == 0 &&
            span.bits64[3] == 0 && span.bits64[0] > 0 &&
            span.bits64[0] < opts.chunk_size) {
          opts.chunk_size = span.bits64[0];
          opts.gpu_stride = std::min(opts.gpu_stride, opts.chunk_size);
        }
      }

      app.options    = opts;
      app.controller = std::make_unique<GuiController>(opts);
      app.last_fs_check = {};
      app.last_fs_matches = UINT64_MAX;
      if (!app.controller->start()) {
        // start() returned false: shouldn't happen after can_edit check,
        // but defensive
        app.controller.reset();
      } else {
        char hms[16] = {0};
        FormatCurrentTimeHms(hms, sizeof(hms));
        std::snprintf(app.last_action_buf, sizeof(app.last_action_buf),
                      "Started at %s", hms);
      }
    } catch (const std::exception& e) {
      std::snprintf(app.last_error_buf, sizeof(app.last_error_buf),
                    "Start failed: %s", e.what());
      app.controller.reset();
    }
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  const bool can_stop = (stats.state == GuiState::Running);
  ImGui::BeginDisabled(!can_stop);
  if (ImGui::Button("Stop", ImVec2(120, 0))) {
    if (app.controller) {
      app.controller->requestStop();
      std::snprintf(app.last_action_buf, sizeof(app.last_action_buf),
                    "Stopped by user");
    }
  }
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextDisabled("%s", app.last_action_buf);

  if (app.last_error_buf[0] != '\0') {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", app.last_error_buf);
  }
}

void DrawTelemetryPanel(AppState& app, const GuiStats& stats) {
  if (stats.state == GuiState::Found && app.prev_state != GuiState::Found) {
    char hms[16] = {0};
    FormatCurrentTimeHms(hms, sizeof(hms));
    std::snprintf(app.last_action_buf, sizeof(app.last_action_buf),
                  "FOUND at %s", hms);
  }
  app.prev_state = stats.state;

  ImGui::Text("Telemetry");
  ImGui::Separator();

  // State with color
  ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  const char* state_str = "Idle";
  switch (stats.state) {
    case GuiState::Idle:      state_str = "Idle";      color = {0.7f, 0.7f, 0.7f, 1.0f}; break;
    case GuiState::Running:   state_str = "Running";   color = {0.2f, 0.8f, 0.2f, 1.0f}; break;
    case GuiState::Stopped:   state_str = "Stopped";   color = {1.0f, 0.7f, 0.2f, 1.0f}; break;
    case GuiState::Found:     state_str = "FOUND";     color = {0.2f, 1.0f, 0.2f, 1.0f}; break;
    case GuiState::Exhausted: state_str = "Exhausted"; color = {1.0f, 0.6f, 0.0f, 1.0f}; break;
    case GuiState::Error:     state_str = "ERROR";     color = {1.0f, 0.2f, 0.2f, 1.0f}; break;
  }
  ImGui::TextColored(color, "State: %s", state_str);

  ImGui::Spacing();
  ImGui::Text("Chunks scanned : %llu", static_cast<unsigned long long>(stats.chunks_scanned));
  ImGui::Text("Keys scanned   : %llu", static_cast<unsigned long long>(stats.keys_scanned));
  ImGui::Text("Rate (Primary): %.1f M/s", stats.rate_keys_per_sec / 1e6);
#ifdef PUZZLE71_ENABLE_CUDA
  if (app.controller && app.options.engine == "gpu") {
    ImGui::Text("Rate (Raw GPU) : %.2f GH/s",
                (stats.rate_keys_per_sec * 6.0) / 1e9);
  }
#endif
  ImGui::Text("Matches found  : %llu", static_cast<unsigned long long>(stats.matches_found));
  ImGui::Text("Elapsed        : %.1f s", stats.elapsed_seconds);

  if (stats.state == GuiState::Error && !stats.last_error.empty()) {
    std::snprintf(app.last_error_buf, sizeof(app.last_error_buf),
                  "%s", stats.last_error.c_str());
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                       "Error: %s", stats.last_error.c_str());
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("Output files");
  if (app.controller) {
    const auto now = std::chrono::steady_clock::now();
    if (now - app.last_fs_check >= std::chrono::seconds(1) ||
        stats.matches_found != app.last_fs_matches ||
        stats.state != app.last_fs_state) {
      std::error_code ec;
      app.cached_has_matches =
          std::filesystem::exists(app.controller->foundMatchesPath(), ec);
      app.cached_has_log =
          std::filesystem::exists(app.controller->scannedRangesPath(), ec);
      app.last_fs_check   = now;
      app.last_fs_matches = stats.matches_found;
      app.last_fs_state   = stats.state;
    }

    ImGui::BeginDisabled(!app.cached_has_matches);
    if (ImGui::Button("Open Matches")) {
      ShellExecuteA(NULL, "open",
                    app.controller->foundMatchesPath().string().c_str(),
                    NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!app.cached_has_log);
    if (ImGui::Button("Open Log")) {
      ShellExecuteA(NULL, "open",
                    app.controller->scannedRangesPath().string().c_str(),
                    NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Open Output Folder")) {
      ShellExecuteA(NULL, "open",
                    app.controller->foundMatchesPath().parent_path().string().c_str(),
                    NULL, NULL, SW_SHOWNORMAL);
    }
  } else {
    ImGui::TextDisabled("(no active run)");
  }

  // On Found: big visual call-to-action
  if (stats.state == GuiState::Found) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
                       "*** FULL MATCH FOUND ***");
    ImGui::Text("See found_matches.jsonl for the private key.");
  }
}

}  // namespace

int main(int, char**) {
  WNDCLASSEXW wc = {
      sizeof(wc),
      CS_CLASSDC,
      WndProc,
      0L,
      0L,
      GetModuleHandle(nullptr),
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      L"Puzzle71SolverGui",
      nullptr};
  ::RegisterClassExW(&wc);
  HWND hwnd = ::CreateWindowW(
      wc.lpszClassName,
      L"Puzzle 71 Solver",
      WS_OVERLAPPEDWINDOW,
      100, 100, 1024, 560,
      nullptr, nullptr, wc.hInstance, nullptr);

  if (!CreateDeviceD3D(hwnd)) {
    CleanupDeviceD3D();
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  ::ShowWindow(hwnd, SW_SHOWDEFAULT);
  ::UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();

  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  AppState app;
  const ImVec4 clear_color = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);

  bool done = false;
  while (!done) {
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
      if (msg.message == WM_QUIT) {
        done = true;
      }
    }
    if (done) {
      break;
    }

    if (g_SwapChainOccluded &&
        g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
      ::Sleep(10);
      continue;
    }
    g_SwapChainOccluded = false;

    if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
      CleanupRenderTarget();
      g_pSwapChain->ResizeBuffers(
          0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
      g_ResizeWidth = g_ResizeHeight = 0;
      CreateRenderTarget();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      done = true;
    }

    // 1. Poll controller for live telemetry
    GuiStats stats;
    if (app.controller) {
      stats = app.controller->getStats();
    } else {
      stats.state = GuiState::Idle;
    }

    // 2. Central window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Puzzle 71 Solver",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // 3. Two-column layout
    ImGui::BeginChild("LeftPanel",
                      ImVec2(ImGui::GetContentRegionAvail().x * 0.45f, 0),
                      true);
    DrawConfigPanel(app, stats);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("RightPanel", ImVec2(0, 0), true);
    DrawTelemetryPanel(app, stats);
    ImGui::EndChild();

    ImGui::End();  // central window

    ImGui::Render();
    const float clear_color_with_alpha[4] = {
        clear_color.x * clear_color.w,
        clear_color.y * clear_color.w,
        clear_color.z * clear_color.w,
        clear_color.w};
    g_pd3dDeviceContext->OMSetRenderTargets(
        1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(
        g_mainRenderTargetView, clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    HRESULT hr = g_pSwapChain->Present(1, 0);
    g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
  }

  // Window close / ESC cleanup: gracefully stop and join worker thread
  if (app.controller) {
    app.controller->requestStop();
    app.controller->wait();
  }
  app.controller.reset();

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  CleanupDeviceD3D();
  ::DestroyWindow(hwnd);
  ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

  return 0;
}

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
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

#include <future>
#include <map>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "GpuEngine.h"
#include "GuiController.h"
#include "PuzzleDatabase.h"
#include "NetworkAPI.h"
#include "TargetListLoader.h"
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
  char last_action_buf[128] = "Son eylem: yok";
  GuiState prev_state      = GuiState::Idle;
  // Throttled file existence cache (polled at most 1 Hz or on state/match change)
  std::chrono::steady_clock::time_point last_fs_check{};
  uint64_t last_fs_matches = UINT64_MAX;
  GuiState last_fs_state   = GuiState::Idle;
  bool cached_has_matches  = false;
  bool cached_has_log      = false;

  // Multi-address list
  std::vector<std::string> loaded_addresses;
  char address_file_name[MAX_PATH] = "";

  // Puzzles and Balance Intelligence
  struct FoundPrefixEntry {
    std::string address;
    uint32_t tx_count = 0;
    uint64_t balance_satoshis = 0;
    bool checked = false;
  };

  std::vector<PuzzleEntry> puzzles;
  int selected_puzzle_idx = -1;
  std::future<std::map<std::string, AddressInfo>> balance_future;
  bool checking_balance = false;
  std::string balance_status = "Kontrol edilmedi";
  bool is_funded = false;
  AddressInfo current_address_info{};

  // Prefix match background checking
  std::future<std::map<std::string, AddressInfo>> prefix_balance_future;
  bool checking_prefix_balance = false;
  std::vector<std::string> pending_check_prefixes;
  std::vector<FoundPrefixEntry> recent_found_prefixes;
  std::vector<std::string> funded_prefixes_found;
  bool alert_funded_prefix = false;
};

void DrawConfigPanel(AppState& app, const GuiStats& stats) {
  ImGui::Text("Yapılandırma");
  ImGui::Separator();

  const bool can_edit = (stats.state == GuiState::Idle ||
                         stats.state == GuiState::Stopped ||
                         stats.state == GuiState::Found ||
                         stats.state == GuiState::Exhausted ||
                         stats.state == GuiState::Error);

  ImGui::BeginDisabled(!can_edit);

  if (!app.puzzles.empty()) {
    std::string preview = (app.selected_puzzle_idx >= 0 && app.selected_puzzle_idx < static_cast<int>(app.puzzles.size()))
                          ? "Puzzle #" + std::to_string(app.puzzles[app.selected_puzzle_idx].id) + " (" + std::to_string(app.puzzles[app.selected_puzzle_idx].bits) + " bit)"
                          : "Bitcoin Puzzle Seçiniz...";
    if (ImGui::BeginCombo("Puzzle Önayarı", preview.c_str())) {
      for (int i = 0; i < static_cast<int>(app.puzzles.size()); ++i) {
        const bool is_selected = (app.selected_puzzle_idx == i);
        std::string label = "Puzzle #" + std::to_string(app.puzzles[i].id) + " (" + std::to_string(app.puzzles[i].bits) + " bit, " + std::to_string(app.puzzles[i].reward) + " BTC)";
        if (ImGui::Selectable(label.c_str(), is_selected)) {
          app.selected_puzzle_idx = i;
          const auto& p = app.puzzles[i];
          std::snprintf(app.address_buf, sizeof(app.address_buf), "%s", p.address.c_str());
          std::snprintf(app.range_start_buf, sizeof(app.range_start_buf), "%s", p.start_hex.c_str());
          std::snprintf(app.range_end_buf, sizeof(app.range_end_buf), "%s", p.end_hex.c_str());
          // Auto trigger async balance check on selection
          if (!p.address.empty()) {
            app.checking_balance = true;
            app.balance_status = "Bakiye sorgulanıyor...";
            std::string addr = p.address;
            app.balance_future = std::async(std::launch::async, [addr]() {
              return NetworkAPI::QueryBalances({addr});
            });
          }
        }
        if (is_selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
  }

  if (ImGui::SmallButton("Puzzle 71 Aralığını Yükle")) {
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
  if (ImGui::SmallButton("Demo Aralığı AB00:ABFF")) {
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
  if (ImGui::SmallButton("Ayar Dosyasından Yükle")) {
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
                    "Yüklendi: %s", cfg_path.string().c_str());
    } catch (const std::exception& e) {
      std::snprintf(app.last_error_buf, sizeof(app.last_error_buf),
                    "Ayar dosyası yüklenemedi: %s", e.what());
    }
  }
  ImGui::InputText("Hedef Adres", app.address_buf, sizeof(app.address_buf));
  ImGui::SameLine();
  if (ImGui::SmallButton("Bakiye Sorgula")) {
    if (std::strlen(app.address_buf) > 0) {
      app.checking_balance = true;
      app.balance_status = "blockchain.info sorgulanıyor...";
      std::string addr = app.address_buf;
      app.balance_future = std::async(std::launch::async, [addr]() {
        return NetworkAPI::QueryBalances({addr});
      });
    }
  }

  // Multi-address list file selector
  if (ImGui::Button("Adres Dosyası Yükle (.txt)")) {
    char szFile[MAX_PATH] = "";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "Metin Dosyaları (*.txt)\0*.txt\0Tüm Dosyalar (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
      try {
        app.loaded_addresses = puzzle::TargetListLoader::loadAddresses(szFile);
        std::snprintf(app.address_file_name, sizeof(app.address_file_name), "%s",
                      std::filesystem::path(szFile).filename().string().c_str());
        std::snprintf(app.last_action_buf, sizeof(app.last_action_buf),
                      "Yüklendi: %zu adres (%s)", app.loaded_addresses.size(), app.address_file_name);
      } catch (const std::exception& e) {
        std::snprintf(app.last_error_buf, sizeof(app.last_error_buf),
                      "Dosya yükleme hatası: %s", e.what());
      }
    }
  }
  if (!app.loaded_addresses.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Yüklü: %zu adres (%s)",
                       app.loaded_addresses.size(), app.address_file_name);
    ImGui::SameLine();
    if (ImGui::SmallButton("Temizle")) {
      app.loaded_addresses.clear();
      app.address_file_name[0] = '\0';
    }
  }

  // Poll balance future
  if (app.checking_balance && app.balance_future.valid()) {
    if (app.balance_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      try {
        auto res = app.balance_future.get();
        app.checking_balance = false;
        std::string addr = app.address_buf;
        if (res.find(addr) != res.end()) {
          const auto& info = res[addr];
          app.current_address_info = info;
          if (info.balance_satoshis > 0) {
            app.is_funded = true;
            double btc = static_cast<double>(info.balance_satoshis) / 1e8;
            char btc_str[80];
            std::snprintf(btc_str, sizeof(btc_str), "BAKİYELİ! %.8f BTC (%u işlem)", btc, info.tx_count);
            app.balance_status = btc_str;
          } else if (info.tx_count > 0) {
            app.is_funded = false;
            app.balance_status = "Kullanılmış (" + std::to_string(info.tx_count) + " işlem) - Boş";
          } else {
            app.is_funded = false;
            app.balance_status = "Yeni / Kullanılmamış";
          }
        } else {
          app.balance_status = "Adres bulunamadı";
        }
      } catch (const std::exception& e) {
        app.checking_balance = false;
        app.balance_status = std::string("Sorgulama başarısız: ") + e.what();
      }
    }
  }

  if (app.is_funded) {
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Durum: %s", app.balance_status.c_str());
  } else if (app.current_address_info.tx_count > 0) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Durum: %s", app.balance_status.c_str());
  } else {
    ImGui::TextDisabled("Durum: %s", app.balance_status.c_str());
  }

  ImGui::InputText("Başlangıç Aralığı (hex)", app.range_start_buf, sizeof(app.range_start_buf));
  ImGui::InputText("Bitiş Aralığı (hex)", app.range_end_buf, sizeof(app.range_end_buf));
  ImGui::InputScalar("Parça Boyutu (Chunk)", ImGuiDataType_U64, &app.chunk_size_u64);
  ImGui::TextDisabled("(CPU: 10M-50M tavsiye edilir. GPU: 100M-1B tavsiye edilir.)");
  ImGui::InputInt("GPU Adımı (Stride)", &app.gpu_stride_int);
  const char* engines[] = {"cpu", "gpu"};
#ifdef PUZZLE71_ENABLE_CUDA
  if (ImGui::Combo("Hesaplama Motoru", &app.engine_idx, engines, 2)) {
    if (app.engine_idx == 1 && app.chunk_size_u64 == 10'000'000ULL) {
      app.chunk_size_u64 = 100'000'000ULL;
    } else if (app.engine_idx == 0 && app.chunk_size_u64 == 100'000'000ULL) {
      app.chunk_size_u64 = 10'000'000ULL;
    }
  }
#else
  // gpu option visible but disabled
  if (ImGui::Combo("Hesaplama Motoru", &app.engine_idx, engines, 2)) {
    if (app.engine_idx == 1) {
      app.engine_idx = 0;
    }
  }
  if (app.engine_idx == 1) {
    app.engine_idx = 0;
  }
  ImGui::TextDisabled("(Yalnızca CPU derlemesi. GPU için -DPUZZLE71_BUILD_CUDA=ON ile derleyin)");
#endif
  ImGui::InputText("Çıktı Klasörü", app.output_dir_buf, sizeof(app.output_dir_buf));
  ImGui::InputText("Hex Önek Filtresi (csv)", app.prefix_hex_buf, sizeof(app.prefix_hex_buf));
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::Separator();

  // Start button: enabled only when can_edit AND no controller running
  const bool can_start = can_edit;
  ImGui::BeginDisabled(!can_start);
  if (ImGui::Button("Başlat", ImVec2(120, 0))) {
    app.last_error_buf[0] = '\0';
    try {
      if (app.chunk_size_u64 == 0 || app.gpu_stride_int <= 0) {
        throw std::invalid_argument("chunk_size ve gpu_stride > 0 olmalıdır");
      }
      // Build CliOptions from UI state
      puzzle::cli::CliOptions opts;
      opts.address        = app.address_buf;
      opts.address_list   = app.loaded_addresses;
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
                      "Başlatıldı: %s", hms);
      }
    } catch (const std::exception& e) {
      std::snprintf(app.last_error_buf, sizeof(app.last_error_buf),
                    "Başlatma başarısız: %s", e.what());
      app.controller.reset();
    }
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  const bool can_stop = (stats.state == GuiState::Running);
  ImGui::BeginDisabled(!can_stop);
  if (ImGui::Button("Durdur", ImVec2(120, 0))) {
    if (app.controller) {
      app.controller->requestStop();
      std::snprintf(app.last_action_buf, sizeof(app.last_action_buf),
                    "Kullanıcı tarafından durduruldu");
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
                  "BULUNDU: %s", hms);
  }
  app.prev_state = stats.state;

  ImGui::Text("Telemetri & İstatistikler");
  ImGui::Separator();

  // State with color
  ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  const char* state_str = "Beklemede";
  switch (stats.state) {
    case GuiState::Idle:      state_str = "Beklemede";          color = {0.7f, 0.7f, 0.7f, 1.0f}; break;
    case GuiState::Running:   state_str = "Çalışıyor";           color = {0.2f, 0.8f, 0.2f, 1.0f}; break;
    case GuiState::Stopped:   state_str = "Durduruldu";         color = {1.0f, 0.7f, 0.2f, 1.0f}; break;
    case GuiState::Found:     state_str = "BULUNDU!";           color = {0.2f, 1.0f, 0.2f, 1.0f}; break;
    case GuiState::Exhausted: state_str = "Tarama Tamamlandı";  color = {1.0f, 0.6f, 0.0f, 1.0f}; break;
    case GuiState::Error:     state_str = "HATA";               color = {1.0f, 0.2f, 0.2f, 1.0f}; break;
  }
  ImGui::TextColored(color, "Durum: %s", state_str);

  ImGui::Spacing();
  ImGui::Text("Hedef Aralık        : %s - %s", app.range_start_buf, app.range_end_buf);
  ImGui::Text("Toplam Anahtar Alanı : %s", stats.total_keyspace_str.empty() ? "-" : stats.total_keyspace_str.c_str());
  ImGui::Text("Taranan Anahtar      : %llu (%%%.6f)",
              static_cast<unsigned long long>(stats.keys_scanned), stats.progress_percent);
  ImGui::ProgressBar(static_cast<float>(stats.progress_percent / 100.0), ImVec2(-1.0f, 0.0f));

  ImGui::Spacing();
  ImGui::Text("Taranan Parça (Chunk): %llu", static_cast<unsigned long long>(stats.chunks_scanned));
  ImGui::Text("Hız (Birincil)       : %.1f M/s", stats.rate_keys_per_sec / 1e6);
#ifdef PUZZLE71_ENABLE_CUDA
  if (app.controller && app.options.engine == "gpu") {
    ImGui::Text("Hız (Ham GPU)        : %.2f GH/s",
                (stats.rate_keys_per_sec * 6.0) / 1e9);
  }
#endif
  ImGui::Text("Bulunan Eşleşmeler   : %llu", static_cast<unsigned long long>(stats.matches_found));
  ImGui::Text("Geçen Süre           : %.1f s", stats.elapsed_seconds);

  if (!stats.current_range_start.empty() && !stats.current_range_end.empty()) {
    ImGui::Text("Anlık Parça Aralığı  : %s : %s",
                stats.current_range_start.c_str(),
                stats.current_range_end.c_str());
  }
  if (stats.state == GuiState::Running && stats.current_chunk_progress > 0.0) {
    ImGui::ProgressBar(static_cast<float>(stats.current_chunk_progress),
                       ImVec2(-1.0f, 0.0f));
  }

  if (app.alert_funded_prefix) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                       "*** BAKİYELİ ÖNEK BULUNDU! prefix_found.txt dosyasını kontrol edin ***");
    for (const auto& fa : app.funded_prefixes_found) {
      ImGui::BulletText("%s", fa.c_str());
    }
  }

  if (stats.state == GuiState::Error && !stats.last_error.empty()) {
    std::snprintf(app.last_error_buf, sizeof(app.last_error_buf),
                  "%s", stats.last_error.c_str());
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                       "Hata: %s", stats.last_error.c_str());
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("Çıktı Dosyaları");
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
    if (ImGui::Button("Eşleşmeleri Aç (JSONL)")) {
      ShellExecuteA(NULL, "open",
                    app.controller->foundMatchesPath().string().c_str(),
                    NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::EndDisabled();

    std::error_code ec_pfx;
    bool has_prefix_matches = std::filesystem::exists(app.controller->foundMatchesPath().parent_path() / "prefix_found.txt", ec_pfx);
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_prefix_matches);
    if (ImGui::Button("Önek Listesini Aç (TXT)")) {
      ShellExecuteA(NULL, "open",
                    (app.controller->foundMatchesPath().parent_path() / "prefix_found.txt").string().c_str(),
                    NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!app.cached_has_log);
    if (ImGui::Button("Tarama Günlüğünü Aç")) {
      ShellExecuteA(NULL, "open",
                    app.controller->scannedRangesPath().string().c_str(),
                    NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Çıktı Klasörünü Aç")) {
      ShellExecuteA(NULL, "open",
                    app.controller->foundMatchesPath().parent_path().string().c_str(),
                    NULL, NULL, SW_SHOWNORMAL);
    }
  } else {
    ImGui::TextDisabled("(aktif çalışan görev yok)");
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("Son Bulunan Vanity Önekleri (%zu)", app.recent_found_prefixes.size());
  if (app.recent_found_prefixes.empty()) {
    ImGui::TextDisabled("Henüz hiçbir vanity öneki bulunamadı.");
  } else {
    if (ImGui::BeginTable("RecentPrefixesTable", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 150))) {
      ImGui::TableSetupColumn("Adres", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("İşlem Sayısı", ImGuiTableColumnFlags_WidthFixed, 85.0f);
      ImGui::TableSetupColumn("Bakiye / Durum", ImGuiTableColumnFlags_WidthFixed, 150.0f);
      ImGui::TableHeadersRow();

      for (int i = static_cast<int>(app.recent_found_prefixes.size()) - 1; i >= 0; --i) {
        const auto& item = app.recent_found_prefixes[i];
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(item.address.c_str());

        ImGui::TableSetColumnIndex(1);
        if (!item.checked) {
          ImGui::TextDisabled("Sorgulanıyor...");
        } else {
          ImGui::Text("%u", item.tx_count);
        }

        ImGui::TableSetColumnIndex(2);
        if (!item.checked) {
          ImGui::TextDisabled("Sorgulanıyor...");
        } else if (item.balance_satoshis > 0) {
          double btc = static_cast<double>(item.balance_satoshis) / 1e8;
          ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%.8f BTC", btc);
        } else if (item.tx_count > 0) {
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Kullanılmış (Boş)");
        } else {
          ImGui::TextDisabled("Yeni / 0 BTC");
        }
      }
      ImGui::EndTable();
    }
  }

  // On Found: big visual call-to-action
  if (stats.state == GuiState::Found) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
                       "*** HEDEF ADRES EŞLEŞMESİ BULUNDU ***");
    ImGui::Text("Özel anahtar (private key) found_matches.jsonl dosyasına kaydedildi.");
  }
}

const ImWchar* GetGlyphRangesTurkish() {
  static const ImWchar ranges[] = {
      0x0020, 0x017F,  // Basic Latin, Latin-1 Supplement, Latin Extended-A (covers Turkish çÇğĞıİöÖşŞüÜ)
      0x20BA, 0x20BA,  // Turkish Lira symbol (₺)
      0,
  };
  return ranges;
}

}  // namespace

int main(int, char**) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
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
      L"Bitcoin Puzzle & Vanity Çözücü",
      WS_OVERLAPPEDWINDOW,
      100, 100, 1080, 680,
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

  // Load Turkish Unicode font (Segoe UI / Tahoma / Arial)
  ImFont* font = nullptr;
  const char* font_candidates[] = {
      "C:\\Windows\\Fonts\\segoeui.ttf",
      "C:\\Windows\\Fonts\\tahoma.ttf",
      "C:\\Windows\\Fonts\\arial.ttf"
  };
  for (const char* font_path : font_candidates) {
    std::error_code ec;
    if (std::filesystem::exists(font_path, ec)) {
      font = io.Fonts->AddFontFromFileTTF(font_path, 16.0f, nullptr, GetGlyphRangesTurkish());
      if (font) {
        break;
      }
    }
  }
  if (!font) {
    std::fprintf(stderr, "Uyarı: Türkçe Unicode yazı tipi yüklenemedi, varsayılan fonta dönülüyor.\n");
    io.Fonts->AddFontDefault();
  }
  io.Fonts->Build();

  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  AppState app;
  try {
    app.puzzles = PuzzleDatabase::load("");
    for (size_t i = 0; i < app.puzzles.size(); ++i) {
      if (app.puzzles[i].id == 71) {
        app.selected_puzzle_idx = static_cast<int>(i);
        break;
      }
    }
    if (app.selected_puzzle_idx >= 0) {
      const auto& p = app.puzzles[app.selected_puzzle_idx];
      std::snprintf(app.address_buf, sizeof(app.address_buf), "%s", p.address.c_str());
      std::snprintf(app.range_start_buf, sizeof(app.range_start_buf), "%s", p.start_hex.c_str());
      std::snprintf(app.range_end_buf, sizeof(app.range_end_buf), "%s", p.end_hex.c_str());
    }
  } catch (...) {
  }
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

    // 1. Poll controller for live telemetry & pop newly discovered prefixes
    GuiStats stats;
    if (app.controller) {
      stats = app.controller->getStats();
      auto new_pfx = app.controller->popFoundPrefixes();
      for (const auto& addr : new_pfx) {
        app.pending_check_prefixes.push_back(addr);
        AppState::FoundPrefixEntry entry;
        entry.address = addr;
        entry.checked = false;
        app.recent_found_prefixes.push_back(entry);
        if (app.recent_found_prefixes.size() > 50) {
          app.recent_found_prefixes.erase(app.recent_found_prefixes.begin());
        }
      }
    } else {
      stats.state = GuiState::Idle;
    }

    // Process pending prefixes asynchronously with blockchain.info
    if (!app.checking_prefix_balance && !app.pending_check_prefixes.empty()) {
      std::vector<std::string> to_check = std::move(app.pending_check_prefixes);
      app.pending_check_prefixes.clear();
      app.checking_prefix_balance = true;
      app.prefix_balance_future = std::async(std::launch::async, [to_check]() {
        return NetworkAPI::QueryBalances(to_check);
      });
    }

    if (app.checking_prefix_balance && app.prefix_balance_future.valid()) {
      if (app.prefix_balance_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        try {
          auto res = app.prefix_balance_future.get();
          for (const auto& kv : res) {
            const auto& addr = kv.first;
            const auto& info = kv.second;
            for (auto& item : app.recent_found_prefixes) {
              if (item.address == addr) {
                item.balance_satoshis = info.balance_satoshis;
                item.tx_count = info.tx_count;
                item.checked = true;
              }
            }
            if (info.balance_satoshis > 0) {
              app.funded_prefixes_found.push_back(addr);
              app.alert_funded_prefix = true;
            }
          }
        } catch (...) {
        }
        app.checking_prefix_balance = false;
      }
    }

    // 2. Central window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Bitcoin Puzzle & Vanity Çözücü",
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

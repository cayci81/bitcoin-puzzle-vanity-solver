#include "Localization.h"

namespace puzzle {

namespace {

const std::unordered_map<std::string, std::pair<std::string, std::string>> g_strings = {
    // Window & Panels
    {"app_title", {"Bitcoin Puzzle & Vanity Çözücü", "Bitcoin Puzzle & Vanity Solver"}},
    {"panel_config", {"Yapılandırma", "Configuration"}},
    {"panel_telemetry", {"Telemetri & İstatistikler", "Telemetry & Statistics"}},
    {"lang_label", {"Dil / Language", "Language / Dil"}},

    // Presets & Buttons
    {"preset_combo", {"Puzzle Önayarı", "Puzzle Preset"}},
    {"preset_hint", {"Bitcoin Puzzle Seçiniz...", "Select Bitcoin Puzzle..."}},
    {"btn_load_p71", {"Puzzle 71 Aralığını Yükle", "Load Puzzle 71 Range"}},
    {"btn_load_demo", {"Demo Aralığı AB00:ABFF", "Demo Range AB00:ABFF"}},
    {"btn_load_cfg", {"Ayar Dosyasından Yükle", "Load from Config File"}},

    // Target inputs
    {"lbl_target_address", {"Hedef Adres", "Target Address"}},
    {"btn_check_balance", {"Bakiye Sorgula", "Check Balance"}},
    {"btn_load_address_file", {"Adres Dosyası Yükle (.txt)", "Load Address File (.txt)"}},
    {"filter_text_files", {"Metin Dosyaları (*.txt)\0*.txt\0Tüm Dosyalar (*.*)\0*.*\0", "Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0"}},
    {"btn_clear", {"Temizle", "Clear"}},
    {"lbl_loaded_addresses", {"Yüklü: %zu adres (%s)", "Loaded: %zu addresses (%s)"}},

    // Ranges & Params
    {"lbl_range_start", {"Başlangıç Aralığı (hex)", "Start Range (hex)"}},
    {"lbl_range_end", {"Bitiş Aralığı (hex)", "End Range (hex)"}},
    {"lbl_chunk_size", {"Parça Boyutu (Chunk)", "Chunk Size"}},
    {"lbl_chunk_hint", {"(CPU: 10M-50M tavsiye edilir. GPU: 100M-1B tavsiye edilir.)", "(CPU: 10M-50M recommended. GPU: 100M-1B recommended.)"}},
    {"lbl_gpu_stride", {"GPU Adımı (Stride)", "GPU Stride"}},
    {"lbl_engine", {"Hesaplama Motoru", "Compute Engine"}},
    {"lbl_cpu_only_hint", {"(Yalnızca CPU derlemesi. GPU için -DPUZZLE71_BUILD_CUDA=ON ile derleyin)", "(CPU-only build. For GPU rebuild with -DPUZZLE71_BUILD_CUDA=ON)"}},
    {"lbl_gpu_grid_x", {"GPU Blok Sayısı (Grid X)", "GPU Block Count (Grid X)"}},
    {"lbl_gpu_grid_x_hint", {"(-1 = Otomatik: SM x 8)", "(-1 = Auto: SM x 8)"}},
    {"lbl_gpu_grid_y", {"GPU Thread Sayısı (Grid Y)", "GPU Thread Count (Grid Y)"}},
    {"lbl_gpu_grid_y_hint", {"(Varsayılan: 128)", "(Default: 128)"}},
    {"btn_optimize_rtx4060", {"RTX 4060 İçin Optimize Et", "Optimize for RTX 4060"}},
    {"lbl_output_dir", {"Çıktı Klasörü", "Output Directory"}},
    {"lbl_prefix_hex_filter", {"Hex Önek Filtresi (csv)", "Hex Prefix Filter (csv)"}},

    // Controls
    {"btn_start", {"Başlat", "Start"}},
    {"btn_stop", {"Durdur", "Stop"}},
    {"action_none", {"Son eylem: yok", "Last action: none"}},
    {"action_stopped_user", {"Kullanıcı tarafından durduruldu", "Stopped by user"}},
    {"action_started", {"Başlatıldı: %s", "Started: %s"}},
    {"action_start_failed", {"Başlatma başarısız: %s", "Start failed: %s"}},
    {"action_found", {"BULUNDU: %s", "FOUND: %s"}},
    {"action_file_loaded", {"Yüklendi: %zu adres (%s)", "Loaded: %zu addresses (%s)"}},
    {"action_file_error", {"Dosya yükleme hatası: %s", "File load error: %s"}},
    {"action_cfg_loaded", {"Yüklendi: %s", "Loaded: %s"}},
    {"action_cfg_error", {"Ayar dosyası yüklenemedi: %s", "Config load failed: %s"}},
    {"action_fallback_cpu", {"Uyarı: 4096'dan fazla adres yüklendi, CPU motoruna geçildi.", "Warning: > 4096 addresses loaded, switched to CPU engine."}},
    {"err_params_positive", {"chunk_size ve gpu_stride > 0 olmalıdır", "chunk_size and gpu_stride must be > 0"}},

    // Balance states
    {"bal_querying", {"blockchain.info sorgulanıyor...", "Querying blockchain.info..."}},
    {"bal_status_prefix", {"Durum: %s", "Status: %s"}},
    {"bal_funded", {"BAKİYELİ! %.8f BTC (%u işlem)", "FUNDED! %.8f BTC (%u txs)"}},
    {"bal_used_empty", {"Kullanılmış (%u işlem) - Boş", "Used (%u txs) - Empty"}},
    {"bal_new", {"Yeni / Kullanılmamış", "New / Unused"}},
    {"bal_not_found", {"Adres bulunamadı", "Address not found"}},
    {"bal_failed", {"Sorgulama başarısız: ", "Query failed: "}},
    {"bal_uninspected", {"Kontrol edilmedi", "Not inspected"}},

    // Telemetry & States
    {"state_idle", {"Beklemede", "Idle"}},
    {"state_running", {"Çalışıyor", "Running"}},
    {"state_stopped", {"Durduruldu", "Stopped"}},
    {"state_found", {"BULUNDU!", "FOUND!"}},
    {"state_exhausted", {"Tarama Tamamlandı", "Search Completed"}},
    {"state_error", {"HATA", "ERROR"}},

    {"tel_state", {"Durum: %s", "Status: %s"}},
    {"tel_target_range", {"Hedef Aralık        : %s - %s", "Target Range        : %s - %s"}},
    {"tel_total_keyspace", {"Toplam Anahtar Alanı : %s", "Total Keyspace      : %s"}},
    {"tel_scanned_keys", {"Taranan Anahtar      : %llu (%%%.6f)", "Keys Scanned        : %llu (%%%.6f)"}},
    {"tel_scanned_chunks", {"Taranan Parça (Chunk): %llu", "Chunks Scanned      : %llu"}},
    {"tel_rate_primary", {"Hız (Birincil)       : %.1f M/s", "Rate (Primary)      : %.1f M/s"}},
    {"tel_rate_raw_gpu", {"Hız (Ham GPU)        : %.2f GH/s", "Rate (Raw GPU)       : %.2f GH/s"}},
    {"tel_matches_found", {"Bulunan Eşleşmeler   : %llu", "Matches Found       : %llu"}},
    {"tel_elapsed_time", {"Geçen Süre           : %.1f s", "Elapsed Time        : %.1f s"}},
    {"tel_current_range", {"Anlık Parça Aralığı  : %s : %s", "Current Chunk Range : %s : %s"}},
    {"tel_random_hint", {"(Arama rastgele aralıklarla yapılır. Aynı aralıkta farklı parçalar taranıyor olabilir)", "(Search is partitioned randomly. Different chunks in range may be scanned)"}},
    {"tel_alert_funded", {"*** BAKİYELİ ÖNEK BULUNDU! prefix_found.txt dosyasını kontrol edin ***", "*** FUNDED PREFIX FOUND! Check prefix_found.txt ***"}},
    {"tel_error", {"Hata: %s", "Error: %s"}},

    // Output files
    {"sec_output_files", {"Çıktı Dosyaları", "Output Files"}},
    {"btn_open_matches", {"Eşleşmeleri Aç (JSONL)", "Open Matches (JSONL)"}},
    {"btn_open_prefixes", {"Önek Listesini Aç (TXT)", "Open Prefix List (TXT)"}},
    {"btn_open_log", {"Tarama Günlüğünü Aç", "Open Scan Log"}},
    {"btn_open_folder", {"Çıktı Klasörünü Aç", "Open Output Folder"}},
    {"lbl_no_active_task", {"(aktif çalışan görev yok)", "(no active task)"}},

    // Recent prefixes table
    {"sec_recent_prefixes", {"Son Bulunan Vanity Önekleri (%zu)", "Recent Vanity Prefixes Found (%zu)"}},
    {"lbl_no_prefixes_yet", {"Henüz hiçbir vanity öneki bulunamadı.", "No vanity prefixes found yet."}},
    {"th_address", {"Adres", "Address"}},
    {"th_tx_count", {"İşlem Sayısı", "Tx Count"}},
    {"th_balance_status", {"Bakiye / Durum", "Balance / Status"}},
    {"tbl_querying", {"Sorgulanıyor...", "Checking..."}},
    {"tbl_used_empty", {"Kullanılmış (Boş)", "Used (Empty)"}},
    {"tbl_new", {"Yeni / 0 BTC", "New / 0 BTC"}},

    // Banner on found
    {"banner_found_title", {"*** HEDEF ADRES EŞLEŞMESİ BULUNDU ***", "*** TARGET ADDRESS MATCH FOUND ***"}},
    {"banner_found_desc", {"Özel anahtar (private key) found_matches.jsonl dosyasına kaydedildi.", "Private key recorded to found_matches.jsonl."}}
};

}  // namespace

const char* Localization::Get(const std::string& key, AppLanguage lang) {
  auto it = g_strings.find(key);
  if (it == g_strings.end()) {
    return key.c_str();
  }
  return (lang == AppLanguage::TR) ? it->second.first.c_str() : it->second.second.c_str();
}

const std::unordered_map<std::string, std::pair<std::string, std::string>>&
Localization::GetAllStrings() {
  return g_strings;
}

}  // namespace puzzle

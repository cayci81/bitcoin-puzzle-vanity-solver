#include "cli.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "Base58.h"
#include "CpuEngine.h"
#include "FoundMatchWriter.h"
#include "GpuEngine.h"
#include "JsonRangeLogger.h"
#include "PrefixFilter.h"
#include "PuzzleSolver.h"
#include "RandomChunkGenerator.h"
#include "RangeKey.h"
#include "SECP256K1.h"
#include "TargetListLoader.h"
#include "PuzzleDatabase.h"
#include "hash/sha256.h"

namespace puzzle::cli {

namespace {

constexpr const char* kSolverVersion = "puzzle71_solver 1.0.0";

std::atomic<std::atomic<bool>*> g_signal_stop_flag{nullptr};

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
  if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
      ctrl_type == CTRL_CLOSE_EVENT) {
    std::atomic<bool>* stop_flag =
        g_signal_stop_flag.load(std::memory_order_acquire);
    if (stop_flag != nullptr) {
      stop_flag->store(true, std::memory_order_release);
    }
    return TRUE;
  }
  return FALSE;
}
#else
void PosixSignalHandler(int /*signum*/) {
  std::atomic<bool>* stop_flag =
      g_signal_stop_flag.load(std::memory_order_acquire);
  if (stop_flag != nullptr) {
    stop_flag->store(true, std::memory_order_release);
  }
}
#endif

// Fix 3: RAII guard for OS signal handler registration so g_signal_stop_flag
// never dangles if an exception escapes RunContext.
class SignalHandlerGuard {
 public:
  explicit SignalHandlerGuard(std::atomic<bool>* stop_flag) {
    g_signal_stop_flag.store(stop_flag, std::memory_order_release);
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, PosixSignalHandler);
    std::signal(SIGTERM, PosixSignalHandler);
#endif
  }

  ~SignalHandlerGuard() {
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
#else
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
#endif
    g_signal_stop_flag.store(nullptr, std::memory_order_release);
  }

  SignalHandlerGuard(const SignalHandlerGuard&) = delete;
  SignalHandlerGuard& operator=(const SignalHandlerGuard&) = delete;
};

void PrintTelemetryLineToStderr(const SolverStats& s,
                                const std::string& engine) {
  std::ostringstream oss;
  oss << std::fixed << "[t=" << std::setprecision(1) << s.elapsed_seconds
      << "s] chunks=" << s.chunks_scanned << " keys=" << s.keys_scanned
      << " primary_rate=" << std::setprecision(1) << (s.rate_keys_per_sec / 1e6)
      << "M/s";
#ifdef PUZZLE71_ENABLE_CUDA
  if (engine == "gpu") {
    // The GPU kernel computes 6 hashes per point (3 endo x 2 parity).
    oss << " raw_rate=" << std::setprecision(2)
        << (s.rate_keys_per_sec * 6.0 / 1e9) << " GH/s";
  }
#else
  (void)engine;
#endif
  oss << " matches=" << s.matches_found << "\n";
  std::cerr << oss.str() << std::flush;
}

// Fix 5: RAII guard for telemetry background thread so it is always signaled
// and joined before local stack objects (solver, stop_flag) are destroyed.
class TelemetryThreadGuard {
 public:
  TelemetryThreadGuard(PuzzleSolver& solver,
                       std::atomic<bool>& stop_flag,
                       std::atomic<bool>* external_stop,
                       std::string engine)
      : solver_(solver),
        stop_flag_(stop_flag),
        external_stop_(external_stop),
        engine_(std::move(engine)),
        thread_([this]() { runLoop(); }) {}

  ~TelemetryThreadGuard() { stopAndJoin(); }

  void stopAndJoin() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      done_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  TelemetryThreadGuard(const TelemetryThreadGuard&) = delete;
  TelemetryThreadGuard& operator=(const TelemetryThreadGuard&) = delete;

 private:
  void runLoop() {
    auto next_print =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::unique_lock<std::mutex> lk(mu_);
    while (!done_ && !stop_flag_.load(std::memory_order_acquire)) {
      if (external_stop_ != nullptr &&
          external_stop_->load(std::memory_order_acquire)) {
        stop_flag_.store(true, std::memory_order_release);
        break;
      }
      cv_.wait_for(lk, std::chrono::milliseconds(25), [this]() {
        return done_ || stop_flag_.load(std::memory_order_acquire);
      });
      if (external_stop_ != nullptr &&
          external_stop_->load(std::memory_order_acquire)) {
        stop_flag_.store(true, std::memory_order_release);
        break;
      }
      auto now = std::chrono::steady_clock::now();
      if (!done_ && !stop_flag_.load(std::memory_order_acquire) &&
          now >= next_print) {
        lk.unlock();
        PrintTelemetryLineToStderr(solver_.getStats(), engine_);
        next_print = now + std::chrono::seconds(2);
        lk.lock();
      }
    }
  }

  PuzzleSolver& solver_;
  std::atomic<bool>& stop_flag_;
  std::atomic<bool>* external_stop_;
  std::string engine_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread thread_;
};

uint64_t ParseStrictUint64(const std::string& str, const std::string& flag) {
  if (str.empty()) {
    throw std::invalid_argument(flag + ": empty numeric value");
  }
  for (char c : str) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      throw std::invalid_argument(flag + ": invalid unsigned integer '" + str +
                                  "'");
    }
  }
  try {
    size_t idx = 0;
    unsigned long long val = std::stoull(str, &idx, 10);
    if (idx != str.size()) {
      throw std::invalid_argument(flag + ": invalid unsigned integer '" + str +
                                  "'");
    }
    return static_cast<uint64_t>(val);
  } catch (const std::exception&) {
    throw std::invalid_argument(flag + ": integer out of range '" + str + "'");
  }
}

int ParseStrictInt(const std::string& str, const std::string& flag) {
  if (str.empty()) {
    throw std::invalid_argument(flag + ": empty integer value");
  }
  size_t start = (str[0] == '-') ? 1 : 0;
  if (start == str.size()) {
    throw std::invalid_argument(flag + ": invalid integer '" + str + "'");
  }
  for (size_t i = start; i < str.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(str[i]))) {
      throw std::invalid_argument(flag + ": invalid integer '" + str + "'");
    }
  }
  try {
    size_t idx = 0;
    int val = std::stoi(str, &idx, 10);
    if (idx != str.size()) {
      throw std::invalid_argument(flag + ": invalid integer '" + str + "'");
    }
    return val;
  } catch (const std::exception&) {
    throw std::invalid_argument(flag + ": integer out of range '" + str + "'");
  }
}

const char* ExitReasonToString(SolverExitReason reason) {
  switch (reason) {
    case SolverExitReason::Exhausted:
      return "Exhausted";
    case SolverExitReason::FullMatchFound:
      return "FullMatchFound";
    case SolverExitReason::StoppedExternal:
      return "StoppedExternal";
  }
  return "Unknown";
}

}  // namespace

void PrintHelp() {
  std::cerr
      << "Kullanım: puzzle71_solver [seçenekler]\n\n"
      << "Seçenekler:\n"
      << "  --config <dosya>                 TOML yapılandırma dosyasından ayarları yükler\n"
      << "  --address <b58>                  Hedef sıkıştırılmış P2PKH Bitcoin adresi\n"
      << "  --range-hex <başlangıç>:<bitiş>  Onaltılık (hex) anahtar aralığı\n"
      << "  --chunk-size <N>                 Parça başına anahtar sayısı (varsayılan: 100000000)\n"
      << "  --gpu-stride <N>                 GPU adım hizalaması (varsayılan: 262144)\n"
      << "  --max-collision-retries <N>      Rastgele çakışma deneme sınırı (varsayılan: 8)\n"
      << "  --seed <N>                       RNG tohum değeri (0 = sistem saatinden türetilir)\n"
      << "  --output-dir <dizin>             scanned_ranges ve found_matches çıktı dizini\n"
      << "  --engine <cpu|gpu>               Hesaplama motoru (varsayılan: cpu)\n"
      << "  --gpu-id <id>                    CUDA GPU cihaz kimliği (varsayılan: 0)\n"
      << "  --gpu-grid-x <N>                 GPU blok sayısı (-1 = SM x 8 otomatik, varsayılan: -1)\n"
      << "  --gpu-grid-y <N>                 Blok başına GPU thread sayısı (varsayılan: 128)\n"
      << "  --puzzle <id>                    puzzles.json dosyasından bulmaca kimliği (otomatik aralık/adres)\n"
      << "  --max-chunks <N>                 N parça sonra durur (0 = sınırsız)\n"
      << "  --prefix-hex <csv>               Virgülle ayrılmış hash160 hex önekleri (örn. 5abc,6123)\n"
      << "  --prefix-b58 <önek>              Base58 adres öneki (tekrarlanabilir, örn. 1PWo3)\n"
      << "  --address-file <dosya>           P2PKH adresleri listesi içeren metin dosyası (# yorum destekli)\n"
      << "  --prefix-file <dosya>            Hex veya Base58 önekleri listesi içeren metin dosyası\n"
      << "  --help                           Bu yardım mesajını gösterir ve çıkar\n"
      << "  --version                        Sürüm bilgisini gösterir ve çıkar\n";
}

void PrintVersion() {
  std::cout << kSolverVersion << "\n";
}

namespace {

std::string TrimWhitespace(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
    ++b;
  }
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    --e;
  }
  return s.substr(b, e - b);
}

std::string StripTomlComments(const std::string& line) {
  bool in_quotes = false;
  char quote_char = '\0';
  bool escaped = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_quotes) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\' && quote_char == '"') {
        escaped = true;
      } else if (c == quote_char) {
        in_quotes = false;
      }
    } else {
      if (c == '"' || c == '\'') {
        in_quotes = true;
        quote_char = c;
      } else if (c == '#') {
        return line.substr(0, i);
      }
    }
  }
  return line;
}

std::string UnquoteTomlValue(const std::string& val) {
  if (val.size() >= 2 &&
      ((val.front() == '"' && val.back() == '"') ||
       (val.front() == '\'' && val.back() == '\''))) {
    return val.substr(1, val.size() - 2);
  }
  return val;
}

}  // namespace

void LoadConfigFile(const std::filesystem::path& config_path,
                    CliOptions& options,
                    bool* has_range_out) {
  std::ifstream in(config_path);
  if (!in.is_open()) {
    throw std::invalid_argument("cannot open config file: " +
                                config_path.string());
  }

  bool has_start = false;
  bool has_end = false;
  std::string raw_line;
  size_t line_no = 0;

  while (std::getline(in, raw_line)) {
    ++line_no;
    std::string line = TrimWhitespace(StripTomlComments(raw_line));
    if (line.empty()) {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      continue;  // section header
    }
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      throw std::invalid_argument("invalid config line " +
                                  std::to_string(line_no) + " in " +
                                  config_path.string());
    }
    std::string key = TrimWhitespace(line.substr(0, eq));
    std::string val = UnquoteTomlValue(TrimWhitespace(line.substr(eq + 1)));

    if (key == "address") {
      options.address = val;
    } else if (key == "range_start") {
      options.range_start = RangeKey::FromHex(val);
      has_start = true;
    } else if (key == "range_end") {
      options.range_end = RangeKey::FromHex(val);
      has_end = true;
    } else if (key == "chunk_size") {
      uint64_t v = ParseStrictUint64(val, "chunk_size");
      if (v == 0) {
        throw std::invalid_argument("chunk_size must be > 0");
      }
      options.chunk_size = v;
    } else if (key == "gpu_stride") {
      uint64_t v = ParseStrictUint64(val, "gpu_stride");
      if (v == 0) {
        throw std::invalid_argument("gpu_stride must be > 0");
      }
      options.gpu_stride = v;
    } else if (key == "max_collision_retries") {
      options.max_collision_retries =
          static_cast<uint32_t>(ParseStrictUint64(val, "max_collision_retries"));
    } else if (key == "seed") {
      options.seed = ParseStrictUint64(val, "seed");
    } else if (key == "type" || key == "engine") {
      if (val != "cpu" && val != "gpu") {
        throw std::invalid_argument("unknown engine value in config: " + val);
      }
      options.engine = val;
    } else if (key == "gpu_id") {
      options.gpu_id = ParseStrictInt(val, "gpu_id");
    } else if (key == "gpu_grid_x") {
      int v = ParseStrictInt(val, "gpu_grid_x");
      if (v < -1 || v == 0) {
        throw std::invalid_argument("gpu_grid_x must be > 0 or -1");
      }
      options.gpu_grid_x = v;
    } else if (key == "gpu_grid_y") {
      int v = ParseStrictInt(val, "gpu_grid_y");
      if (v <= 0) {
        throw std::invalid_argument("gpu_grid_y must be > 0");
      }
      options.gpu_grid_y = v;
    } else if (key == "dir" || key == "output_dir") {
      options.output_dir = val;
    } else if (key == "prefix_hex") {
      (void)HexPrefixFilter::parse(val);
      options.prefix_hex_csv = val;
    } else if (key == "prefix_b58") {
      options.prefix_b58.clear();
      if (!val.empty()) {
        options.prefix_b58.push_back(val);
      }
    } else if (key == "max_chunks") {
      options.max_chunks = ParseStrictUint64(val, "max_chunks");
    } else {
      throw std::invalid_argument("unknown config key '" + key + "' in " +
                                  config_path.string());
    }
  }

  if (has_start != has_end) {
    throw std::invalid_argument(
        "config file must specify both range_start and range_end, or neither: " +
        config_path.string());
  }
  if (has_start && has_end && has_range_out != nullptr) {
    *has_range_out = true;
  }
}

ParsedArgs ParseArgs(int argc, char** argv) {
  ParsedArgs parsed;
  bool has_range = false;

  auto require_value = [&](int& i, const std::string& flag) -> std::string {
    if (i + 1 >= argc || argv[i + 1] == nullptr) {
      throw std::invalid_argument(flag + " requires a value");
    }
    return std::string(argv[++i]);
  };

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i] ? argv[i] : "";

    if (arg == "--help" || arg == "-h") {
      parsed.show_help = true;
      return parsed;
    }
    if (arg == "--version" || arg == "-v") {
      parsed.show_version = true;
      return parsed;
    }

    if (arg == "--config") {
      std::string cfg_path = require_value(i, arg);
      LoadConfigFile(cfg_path, parsed.options, &has_range);
    } else if (arg == "--address") {
      parsed.options.address = require_value(i, arg);
    } else if (arg == "--range-hex" || arg == "--keyspace") {
      std::string spec = require_value(i, arg);
      auto colon = spec.find(':');
      if (colon == std::string::npos) {
        throw std::invalid_argument(
            "--range-hex missing ':' separator in '" + spec + "'");
      }
      std::string start_hex = spec.substr(0, colon);
      std::string end_hex = spec.substr(colon + 1);
      if (start_hex.empty() || end_hex.empty()) {
        throw std::invalid_argument(
            "--range-hex requires non-empty start and end hex strings");
      }
      parsed.options.range_start = RangeKey::FromHex(start_hex);
      parsed.options.range_end = RangeKey::FromHex(end_hex);
      has_range = true;
    } else if (arg == "--chunk-size") {
      uint64_t val = ParseStrictUint64(require_value(i, arg), arg);
      if (val == 0) {
        throw std::invalid_argument("--chunk-size must be > 0");
      }
      parsed.options.chunk_size = val;
    } else if (arg == "--gpu-stride") {
      uint64_t val = ParseStrictUint64(require_value(i, arg), arg);
      if (val == 0) {
        throw std::invalid_argument("--gpu-stride must be > 0");
      }
      parsed.options.gpu_stride = val;
    } else if (arg == "--max-collision-retries") {
      uint64_t val = ParseStrictUint64(require_value(i, arg), arg);
      parsed.options.max_collision_retries = static_cast<uint32_t>(val);
    } else if (arg == "--seed") {
      parsed.options.seed = ParseStrictUint64(require_value(i, arg), arg);
    } else if (arg == "--output-dir") {
      parsed.options.output_dir = require_value(i, arg);
    } else if (arg == "--engine") {
      // Fix 2: ParseArgs accepts both "cpu" and "gpu"; CUDA availability is
      // checked in BuildSolverContext / RunSolver, not ParseArgs.
      std::string eng = require_value(i, arg);
      if (eng == "cpu" || eng == "gpu") {
        parsed.options.engine = eng;
      } else {
        throw std::invalid_argument("unknown --engine value: " + eng);
      }
    } else if (arg == "--gpu-id") {
      parsed.options.gpu_id = ParseStrictInt(require_value(i, arg), arg);
    } else if (arg == "--gpu-grid-x") {
      int v = ParseStrictInt(require_value(i, arg), arg);
      if (v < -1 || v == 0) {
        throw std::invalid_argument("--gpu-grid-x must be > 0 or -1 for auto");
      }
      parsed.options.gpu_grid_x = v;
    } else if (arg == "--gpu-grid-y") {
      int v = ParseStrictInt(require_value(i, arg), arg);
      if (v <= 0) {
        throw std::invalid_argument("--gpu-grid-y must be > 0");
      }
      parsed.options.gpu_grid_y = v;
    } else if (arg == "--max-chunks") {
      parsed.options.max_chunks = ParseStrictUint64(require_value(i, arg), arg);
    } else if (arg == "--prefix-hex") {
      std::string csv = require_value(i, arg);
      (void)HexPrefixFilter::parse(csv);
      parsed.options.prefix_hex_csv = csv;
    } else if (arg == "--prefix-b58") {
      std::string pfx = require_value(i, arg);
      parsed.options.prefix_b58.push_back(pfx);
      (void)AddressMatcher(parsed.options.prefix_b58, std::nullopt);
    } else if (arg == "--address-file") {
      parsed.options.address_file = require_value(i, arg);
      (void)TargetListLoader::loadAddresses(parsed.options.address_file);
    } else if (arg == "--prefix-file") {
      parsed.options.prefix_file = require_value(i, arg);
      (void)TargetListLoader::loadPrefixes(parsed.options.prefix_file);
    } else if (arg == "--puzzle") {
      int pid = ParseStrictInt(require_value(i, arg), arg);
      if (pid <= 0) {
        throw std::invalid_argument("--puzzle ID must be > 0");
      }
      parsed.options.puzzle_id = pid;
      auto puzzles = PuzzleDatabase::load("");
      const auto* entry = PuzzleDatabase::findById(puzzles, pid);
      if (!entry) {
        throw std::invalid_argument("Puzzle ID " + std::to_string(pid) + " not found in puzzles.json");
      }
      parsed.options.range_start = RangeKey::FromHex(entry->start_hex);
      parsed.options.range_end = RangeKey::FromHex(entry->end_hex);
      if (parsed.options.address.empty()) {
        parsed.options.address = entry->address;
      }
      has_range = true;
    } else {
      throw std::invalid_argument("Unknown flag: " + arg);
    }
  }

  if (!has_range) {
    throw std::invalid_argument("either --keyspace or --range-hex required");
  }
  if (parsed.options.range_end < parsed.options.range_start) {
    throw std::invalid_argument("--range-hex start must be <= end");
  }
  if (parsed.options.chunk_size == 0) {
    throw std::invalid_argument("--chunk-size must be > 0");
  }

  std::optional<std::string> full_addr =
      parsed.options.address.empty()
          ? std::nullopt
          : std::make_optional(parsed.options.address);
  (void)AddressMatcher(parsed.options.prefix_b58, full_addr);

  return parsed;
}

SolverContext::SolverContext(const CliOptions& opts) : options(opts) {
  std::error_code ec;
  std::filesystem::create_directories(options.output_dir, ec);

  // 1. Build Secp256K1 (Init()).
  secp = std::make_shared<Secp256K1>();
  secp->Init();

  // 2. Gather all targets: single flags + file inputs (deduplicated).
  std::vector<std::string> full_addresses;
  auto add_unique = [](std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
  };
  if (!options.address.empty()) add_unique(full_addresses, options.address);
  for (const auto& a : options.address_list) add_unique(full_addresses, a);
  if (!options.address_file.empty()) {
    for (const auto& a : TargetListLoader::loadAddresses(options.address_file)) {
      add_unique(full_addresses, a);
    }
  }

  std::vector<std::string> hex_prefixes =
      HexPrefixFilter::parse(options.prefix_hex_csv).prefixes();
  std::vector<std::string> b58_prefixes;
  for (const auto& p : options.prefix_b58) add_unique(b58_prefixes, p);
  if (!options.prefix_file.empty()) {
    LoadedPrefixes lp = TargetListLoader::loadPrefixes(options.prefix_file);
    for (const auto& h : lp.hex_prefixes) add_unique(hex_prefixes, h);
    for (const auto& b : lp.base58_prefixes) add_unique(b58_prefixes, b);
  }
  HexPrefixFilter hex_filter = HexPrefixFilter::parse(hex_prefixes);

  // Validate Base58Check checksum and P2PKH version byte (0x00) for every
  // full target address; collect hash160 prefix16 slots.
  std::vector<uint16_t> address_prefix16;
  for (const auto& addr : full_addresses) {
    std::vector<unsigned char> decoded_addr;
    if (!DecodeBase58(addr, decoded_addr) || decoded_addr.size() != 25 ||
        decoded_addr[0] != 0x00) {
      throw std::invalid_argument(
          "invalid P2PKH target address (bad length or version byte): " + addr);
    }
    uint8_t expected_checksum[4] = {0};
    sha256_checksum(decoded_addr.data(), 21, expected_checksum);
    if (std::memcmp(expected_checksum, decoded_addr.data() + 21, 4) != 0) {
      throw std::invalid_argument(
          "invalid P2PKH target address (Base58Check checksum mismatch): " + addr);
    }
    const unsigned char* h160 = decoded_addr.data() + 1;  // skip version byte
    address_prefix16.push_back(
        static_cast<uint16_t>(h160[0] | (static_cast<uint16_t>(h160[1]) << 8)));
  }

  // Narrow gpu_table when no hex prefixes are given and only full addresses
  // (no Base58 prefixes) are targeted: set prefix16 of ALL addresses.
  if (hex_prefixes.empty() && !full_addresses.empty() && b58_prefixes.empty()) {
    gpu_table.assign(65536, 0);
    for (uint16_t p16 : address_prefix16) gpu_table[p16] = 1;
  } else if (hex_prefixes.empty() && !b58_prefixes.empty() &&
             full_addresses.empty()) {
    // Base58-prefix-only mode: cannot narrow gpu_table (dense). GpuEngine
    // rejects dense tables, so --engine gpu is not supported in this mode.
    if (options.engine == "gpu") {
      throw std::invalid_argument(
          "--engine gpu requires a specific target: pass --address <addr> "
          "or --prefix-hex <hex> (Base58 prefix alone forces dense table)");
    }
    gpu_table = hex_filter.buildGpuTable();
    warn_dense_b58_only = true;
  } else {
    gpu_table = hex_filter.buildGpuTable();
    // Make sure full addresses are never filtered out by a hex-only table.
    if (!hex_prefixes.empty() && !full_addresses.empty()) {
      for (uint16_t p16 : address_prefix16) gpu_table[p16] = 1;
    }
  }

  // 3. AddressMatcher over all full addresses and Base58 prefixes.
  matcher = std::make_shared<AddressMatcher>(b58_prefixes, full_addresses);

  // 4. RandomChunkGenerator with GeneratorConfig built from options.
  // Fix 4: Do NOT clamp gpu_stride to chunk_size; let RandomChunkGenerator
  // throw std::invalid_argument when chunk_size < gpu_stride.
  GeneratorConfig gen_cfg;
  gen_cfg.range_start = options.range_start;
  gen_cfg.range_end = options.range_end;
  gen_cfg.chunk_size = options.chunk_size;
  gen_cfg.gpu_stride = options.gpu_stride;
  gen_cfg.max_collision_retries = options.max_collision_retries;
  gen_cfg.seed = (options.seed != 0)
                     ? options.seed
                     : static_cast<uint64_t>(
                           std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count());
  generator = std::make_shared<RandomChunkGenerator>(gen_cfg);

  // 5. JsonRangeLogger with LoggerConfig{output_dir, "scanned_ranges", flush_every_n=10, rotate=100MB}.
  LoggerConfig log_cfg;
  log_cfg.directory = options.output_dir;
  log_cfg.base_name = "scanned_ranges";
  log_cfg.flush_every_n = 10;
  log_cfg.rotate_bytes = 100ull * 1024 * 1024;
  logger = std::make_shared<JsonRangeLogger>(log_cfg);

  // 6 & 7. FoundMatchWriter with MatchWriterConfig pointing to this->stop_flag.
  MatchWriterConfig writer_cfg;
  writer_cfg.output_path = options.output_dir / "found_matches.jsonl";
  writer_cfg.stop_flag = &stop_flag;
  writer = std::make_shared<FoundMatchWriter>(writer_cfg);

  // 8. Resume: replay existing log records into generator.
  // resume path — see cli.md for details
  for (auto& rec : logger->records()) {
    generator->markScanned(rec.start, rec.scanned_keys);
  }

  // 9 & 10. Engine and SolverConfig.
  if (options.engine == "cpu") {
    engine = std::make_shared<CpuEngine>(secp);
  } else if (options.engine == "gpu") {
    if (!GpuEngine::isAvailable()) {
      throw std::invalid_argument(
          "GPU engine not compiled in; rebuild with "
          "-DPUZZLE71_BUILD_CUDA=ON");
    }
    try {
      GpuEngineConfig gpu_cfg;
      gpu_cfg.gpu_id = options.gpu_id;
      gpu_cfg.grid_size_x = options.gpu_grid_x;
      gpu_cfg.grid_size_y = options.gpu_grid_y;
      engine = std::make_shared<GpuEngine>(secp, gpu_table, gpu_cfg);
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()).find("popcount") != std::string::npos ||
          std::string(e.what()).find("4096") != std::string::npos ||
          std::string(e.what()).find("önek") != std::string::npos) {
        throw std::runtime_error(
            "GPU motoru tek seferde en fazla 4096 önek arayabilir. Lütfen adres sayısını azaltın veya CPU motorunu kullanın.");
      }
      throw;
    }
  } else {
    throw std::invalid_argument("unknown --engine value: " + options.engine);
  }

  SolverConfig solver_cfg;
  solver_cfg.gpu_table = gpu_table;
  solver_cfg.stop_flag = &stop_flag;
  solver_cfg.worker_id = 0;
  solver_cfg.max_chunks = options.max_chunks;
  solver_cfg.on_prefix_found = options.on_prefix_found;
  if (options.range_start <= options.range_end) {
    Int span = options.range_start.distanceTo(options.range_end);
    span.AddOne();
    solver_cfg.total_keyspace_double = span.ToDouble();
    solver_cfg.total_keyspace_str = span.GetBase10();
    if (span.bits64[1] == 0 && span.bits64[2] == 0 && span.bits64[3] == 0 && span.bits64[4] == 0) {
      solver_cfg.total_keyspace = span.bits64[0];
    } else {
      solver_cfg.total_keyspace = UINT64_MAX;
    }
  }

  solver = std::make_unique<PuzzleSolver>(
      engine, generator, logger, matcher, writer, solver_cfg);
}

SolverContext BuildSolverContext(const CliOptions& options) {
  return SolverContext(options);
}

RunResult RunContext(SolverContext& ctx,
                     std::atomic<bool>* external_stop) {
  const auto& options = ctx.options;

  size_t active_prefixes = 0;
  for (uint8_t b : ctx.gpu_table) {
    if (b != 0) ++active_prefixes;
  }
  uint64_t effective_chunk =
      (options.chunk_size / options.gpu_stride) * options.gpu_stride;

  // 11. Print startup banner to stderr.
  std::cerr << "=== Puzzle 71 Solver ===\n"
            << "Address    : " << options.address << "\n"
            << "Range      : " << options.range_start.ToHex() << " : "
            << options.range_end.ToHex() << "\n"
            << "Engine     : " << options.engine << "\n"
            << "Chunk size : " << options.chunk_size << " (effective "
            << effective_chunk << " after stride rounding)\n"
            << "Output dir : " << options.output_dir.string() << "\n"
            << "gpu_table  : " << active_prefixes << " active prefixes\n";
  if (ctx.warn_dense_b58_only) {
    std::cerr << "WARNING    : Base58-prefix-only mode without --prefix-hex "
                 "scans with all 65536 prefixes active (slow on CPU).\n";
  }
  std::cerr << std::flush;

  // 12. Install signal handler (SIGINT, SIGTERM) -> stop_flag.store(true) via RAII guard.
  SignalHandlerGuard sig_guard(&ctx.stop_flag);

  // 13 & 14. Telemetry thread (via RAII guard) + solver->run().
  RunResult r;
  {
    TelemetryThreadGuard telemetry_guard(
        *ctx.solver, ctx.stop_flag, external_stop, options.engine);
    r.reason = ctx.solver->run();
    telemetry_guard.stopAndJoin();
  }

  ctx.logger->flush();

  r.stats = ctx.solver->getStats();
  PrintTelemetryLineToStderr(r.stats, options.engine);

  if (r.reason == SolverExitReason::Exhausted ||
      r.reason == SolverExitReason::FullMatchFound) {
    r.exit_code = 0;
  } else if (r.reason == SolverExitReason::StoppedExternal) {
    r.exit_code = 130;
  } else {
    r.exit_code = 1;
  }

  // 15. Print final line to stderr.
  std::cerr << "Exit: " << ExitReasonToString(r.reason) << ". See "
            << (options.output_dir / "found_matches.jsonl").string() << "\n"
            << std::flush;

  // 16. return r;
  return r;
}

RunResult RunSolver(const CliOptions& options,
                    std::atomic<bool>* external_stop) {
  SolverContext ctx = BuildSolverContext(options);
  return RunContext(ctx, external_stop);
}

}  // namespace puzzle::cli

#ifndef PUZZLE71_NO_MAIN
int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  try {
    auto parsed = puzzle::cli::ParseArgs(argc, argv);
    if (parsed.show_help) {
      puzzle::cli::PrintHelp();
      return 0;
    }
    if (parsed.show_version) {
      puzzle::cli::PrintVersion();
      return 0;
    }
    auto r = puzzle::cli::RunSolver(parsed.options);
    return r.exit_code;
  } catch (const std::invalid_argument& e) {
    std::cerr << "Error: " << e.what() << "\n";
    puzzle::cli::PrintHelp();
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  }
}
#endif

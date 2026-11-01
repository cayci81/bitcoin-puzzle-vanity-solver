// ============================================================================
// GuiController API Contract (implemented in Micro-Step 2.2)
// ============================================================================
//
// namespace puzzle::gui {
//
// enum class GuiState {
//   Idle,         // no worker thread, ready to start
//   Running,      // worker thread alive, solver looping
//   Stopped,      // user pressed Stop OR external_stop was raised
//   Found,        // worker finished with FullMatchFound
//   Exhausted,    // worker finished with generator exhaustion
//   Error,        // worker threw an exception; see last_error()
// };
//
// struct GuiStats {
//   GuiState    state             = GuiState::Idle;
//   uint64_t    chunks_scanned    = 0;
//   uint64_t    keys_scanned      = 0;
//   double      elapsed_seconds   = 0.0;
//   double      rate_keys_per_sec = 0.0;
//   uint64_t    matches_found     = 0;
//   std::string last_error;       // set only when state == GuiState::Error
// };
//
// class GuiController {
//  public:
//   // Construct with a pre-parsed CliOptions. The controller does NOT own
//   // the CLI parsing; it just drives RunSolver on a worker thread.
//   //
//   // CONTRACT: throws std::invalid_argument if options are self-inconsistent:
//   //   - range_start > range_end
//   //   - chunk_size == 0 or chunk_size < gpu_stride
//   //   - engine not in {"cpu", "gpu"}
//   // This makes GUI Start-button state derivable from options at edit time,
//   // rather than discovering invalid options only when the worker throws.
//   explicit GuiController(puzzle::cli::CliOptions options);
//   ~GuiController();
//
//   GuiController(const GuiController&) = delete;
//   GuiController& operator=(const GuiController&) = delete;
//
//   // Start the worker thread. If already running, no-op (returns false).
//   // Returns true if a new worker was spawned.
//   //
//   // START CONTRACT: single-shot. Once the worker exits (Stopped, Found,
//   // Exhausted, or Error), start() permanently returns false for that
//   // instance. To restart, destroy and reconstruct.
//   bool start();
//
//   // Request stop: sets the internal stop_flag. The worker exits cleanly
//   // at the next polling point. Does NOT block; use wait() to join.
//   // No-op if not running.
//   void requestStop();
//
//   // Block until the worker thread has fully exited. Safe to call from
//   // any state; returns immediately if Idle/Stopped/Found/Exhausted/Error.
//   void wait();
//
//   // Non-blocking stats snapshot. Safe to call every frame at 60fps.
//   GuiStats getStats() const;
//
//   // Path to found_matches.jsonl (for "Open Matches" button in GUI).
//   std::filesystem::path foundMatchesPath() const;
//
//   // Path to scanned_ranges.jsonl (for "Open Log" button).
//   std::filesystem::path scannedRangesPath() const;
//
//  private:
//   // ... implementation deferred to Micro-Step 2.2
// };
//
// }  // namespace puzzle::gui

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "../GuiController.h"
#include "../SECP256K1.h"

namespace fs = std::filesystem;
using namespace puzzle;
using namespace puzzle::cli;
using namespace puzzle::gui;

namespace {

class TempDir {
 public:
  TempDir() {
    auto base = fs::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate = base / ("puzzle71_gui_test_" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
          std::to_string(i));
      std::error_code ec;
      if (fs::create_directory(candidate, ec)) {
        path_ = candidate;
        return;
      }
    }
    throw std::runtime_error("Failed to create temporary directory");
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

std::string CompressedAddressForHexKey(const char* hex_privkey) {
  Secp256K1 secp;
  secp.Init();
  Int k;
  k.SetBase16(const_cast<char*>(hex_privkey));
  Point pub = secp.ComputePublicKey(&k);
  return secp.GetAddress(P2PKH, true, pub);
}

std::vector<nlohmann::json> ReadJsonLines(const fs::path& file_path) {
  std::vector<nlohmann::json> out;
  std::ifstream in(file_path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      out.push_back(nlohmann::json::parse(line));
    }
  }
  return out;
}

class GuiControllerFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    target_addr_abcd_ = CompressedAddressForHexKey("ABCD");
  }

  static CliOptions MakeTinyOptions(const fs::path& output_dir,
                                    const std::string& address) {
    CliOptions opts;
    opts.range_start = RangeKey::FromHex("AB00");
    opts.range_end = RangeKey::FromHex("ABFF");
    opts.chunk_size = 256;
    opts.gpu_stride = 256;
    opts.seed = 1;
    opts.address = address;
    opts.engine = "cpu";
    opts.output_dir = output_dir;
    return opts;
  }

  static std::string target_addr_abcd_;
};

std::string GuiControllerFixture::target_addr_abcd_;

using GuiControllerLifecycle = GuiControllerFixture;
using GuiControllerStats = GuiControllerFixture;
using GuiControllerDestructor = GuiControllerFixture;

}  // namespace

// ============================================================================
// Suite 1: GuiControllerLifecycle (12 tests)
// ============================================================================

TEST_F(GuiControllerLifecycle, InitialStateIsIdle) {
  TempDir temp_dir;
  CliOptions opts = MakeTinyOptions(temp_dir.path(), target_addr_abcd_);

  GuiController controller(opts);

  GuiStats stats = controller.getStats();
  EXPECT_EQ(stats.state, GuiState::Idle);
  EXPECT_EQ(stats.chunks_scanned, 0ULL);
  EXPECT_EQ(stats.keys_scanned, 0ULL);
  EXPECT_EQ(stats.matches_found, 0ULL);
  EXPECT_TRUE(stats.last_error.empty());

  EXPECT_EQ(controller.foundMatchesPath().filename(),
            fs::path("found_matches.jsonl"));
  EXPECT_EQ(controller.scannedRangesPath().filename(),
            fs::path("scanned_ranges.jsonl"));
}

TEST_F(GuiControllerLifecycle, StartTransitionsToRunningAndEventuallyFound) {
  TempDir temp_dir;
  CliOptions opts = MakeTinyOptions(temp_dir.path(), target_addr_abcd_);

  GuiController controller(opts);
  ASSERT_TRUE(controller.start());

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
  GuiStats stats = controller.getStats();
  while (stats.state != GuiState::Found &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stats = controller.getStats();
  }

  controller.wait();
  stats = controller.getStats();

  EXPECT_EQ(stats.state, GuiState::Found);
  EXPECT_EQ(stats.matches_found, 1ULL);

  ASSERT_TRUE(fs::exists(controller.foundMatchesPath()));
  auto match_lines = ReadJsonLines(controller.foundMatchesPath());
  ASSERT_EQ(match_lines.size(), 1U);
  EXPECT_EQ(match_lines[0].at("kind").get<std::string>(), "full");
  EXPECT_EQ(match_lines[0].at("address").get<std::string>(), target_addr_abcd_);
  EXPECT_EQ(match_lines[0].at("private_key_hex").get<std::string>(), "ABCD");
}

TEST_F(GuiControllerLifecycle, StartWhenAlreadyRunningIsNoOp) {
  TempDir temp_dir;
  CliOptions opts;
  opts.range_start = RangeKey::FromHex("100000");
  opts.range_end = RangeKey::FromHex("8FFFFF");
  opts.chunk_size = 524288;
  opts.gpu_stride = 524288;
  opts.seed = 1;
  opts.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  opts.engine = "cpu";
  opts.output_dir = temp_dir.path();

  GuiController controller(opts);
  EXPECT_TRUE(controller.start());
  // CONTRACT: start() is atomic-with-respect-to-thread-creation. If the
  // first call spawned a thread, the second call MUST return false
  // regardless of worker state (Running/Found/Stopped/Exhausted/Error).
  // Implementation must use an `std::atomic<bool> started_{false}` flag,
  // not `state == Running`.
  EXPECT_FALSE(controller.start());
  controller.requestStop();
  controller.wait();
  // After join, start() is still false (single-shot contract).
  EXPECT_FALSE(controller.start());
}

TEST_F(GuiControllerLifecycle, StopTransitionsToStoppedNotFound) {
  TempDir temp_dir;
  CliOptions opts;
  opts.range_start = RangeKey::FromHex("100000");
  opts.range_end = RangeKey::FromHex("8FFFFF");
  opts.chunk_size = 524288;
  opts.gpu_stride = 524288;
  opts.seed = 1;
  opts.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  opts.engine = "cpu";
  opts.output_dir = temp_dir.path();

  GuiController controller(opts);
  ASSERT_TRUE(controller.start());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  controller.requestStop();

  const auto wait_start = std::chrono::steady_clock::now();
  controller.wait();
  const auto wait_elapsed = std::chrono::steady_clock::now() - wait_start;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(wait_elapsed)
                .count(),
            5000);

  GuiStats stats = controller.getStats();
  EXPECT_EQ(stats.state, GuiState::Stopped);
  EXPECT_EQ(stats.matches_found, 0ULL);
}

TEST_F(GuiControllerLifecycle, ExhaustedRangeTransitionsToExhausted) {
  TempDir temp_dir;
  // Target address is Puzzle #71's address (outside [0xAB00, 0xABFF]).
  CliOptions opts = MakeTinyOptions(
      temp_dir.path(), "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU");

  GuiController controller(opts);
  ASSERT_TRUE(controller.start());
  controller.wait();

  GuiStats stats = controller.getStats();
  EXPECT_EQ(stats.state, GuiState::Exhausted);
  EXPECT_EQ(stats.matches_found, 0ULL);
}

#ifndef PUZZLE71_BUILD_CUDA
TEST_F(GuiControllerLifecycle, InvalidEngineOnNonCudaBuildTransitionsToError) {
  TempDir temp_dir;
  CliOptions opts = MakeTinyOptions(temp_dir.path(), target_addr_abcd_);
  opts.engine = "gpu";

  GuiController controller(opts);
  EXPECT_TRUE(controller.start());
  controller.wait();

  GuiStats stats = controller.getStats();
  EXPECT_EQ(stats.state, GuiState::Error);
  EXPECT_NE(stats.last_error.find("GPU engine not compiled in"),
            std::string::npos)
      << "Actual last_error: " << stats.last_error;
}
#endif

TEST_F(GuiControllerLifecycle, RequestStopWhileIdleIsNoOp) {
  TempDir temp_dir;
  CliOptions opts = MakeTinyOptions(temp_dir.path(), target_addr_abcd_);
  GuiController controller(opts);
  // Idle state: requestStop() must not crash, state must stay Idle.
  controller.requestStop();
  EXPECT_EQ(controller.getStats().state, GuiState::Idle);
  // wait() must return immediately.
  const auto t0 = std::chrono::steady_clock::now();
  controller.wait();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(elapsed_ms, 100);
  EXPECT_EQ(controller.getStats().state, GuiState::Idle);
}

TEST_F(GuiControllerLifecycle, WaitCalledTwiceIsIdempotent) {
  TempDir temp_dir;
  CliOptions opts = MakeTinyOptions(temp_dir.path(), target_addr_abcd_);
  GuiController controller(opts);
  ASSERT_TRUE(controller.start());
  controller.wait();
  // Second wait must not crash (std::thread::join is UB on twice).
  controller.wait();
  EXPECT_EQ(controller.getStats().state, GuiState::Found);
}

TEST_F(GuiControllerLifecycle, StartAfterStopIsRejected) {
  // Contract: this controller is single-shot. Once the worker exits (any
  // terminal state: Stopped/Found/Exhausted/Error), start() returns false.
  // Construct a NEW GuiController for a fresh run. GUI "Restart" button
  // creates a new controller; it does not reuse the stopped one.
  TempDir temp_dir;
  CliOptions opts;
  opts.range_start = RangeKey::FromHex("100000");
  opts.range_end = RangeKey::FromHex("8FFFFF");
  opts.chunk_size = 524288;
  opts.gpu_stride = 524288;
  opts.seed = 1;
  opts.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  opts.engine = "cpu";
  opts.output_dir = temp_dir.path();
  GuiController controller(opts);
  ASSERT_TRUE(controller.start());
  controller.requestStop();
  controller.wait();
  EXPECT_EQ(controller.getStats().state, GuiState::Stopped);
  // Second start on a terminal controller: single-shot contract.
  EXPECT_FALSE(controller.start());
}

TEST_F(GuiControllerLifecycle, ConstructorThrowsOnInvalidCliOptions) {
  // CONTRACT DECISION: GuiController validates that its CliOptions are
  // self-consistent at construction time, same as RunSolver would.
  // Specifically:
  //   - range_start <= range_end
  //   - chunk_size > 0, chunk_size >= gpu_stride
  //   - engine in {"cpu", "gpu"}  (unknown string throws)
  // Invalid options must throw std::invalid_argument from the constructor,
  // NOT defer the error to the worker thread.
  CliOptions bad;
  bad.range_start = RangeKey::FromHex("FF");
  bad.range_end   = RangeKey::FromHex("00");
  bad.chunk_size  = 256;
  bad.gpu_stride  = 256;
  bad.engine      = "cpu";
  EXPECT_THROW(GuiController{bad}, std::invalid_argument);

  CliOptions bad_stride;
  bad_stride.range_start = RangeKey::FromHex("1000");
  bad_stride.range_end   = RangeKey::FromHex("10FF");
  bad_stride.chunk_size  = 256;
  bad_stride.gpu_stride  = 512;  // > chunk_size
  bad_stride.engine      = "cpu";
  EXPECT_THROW(GuiController{bad_stride}, std::invalid_argument);

  CliOptions bad_engine;
  bad_engine.range_start = RangeKey::FromHex("1000");
  bad_engine.range_end   = RangeKey::FromHex("10FF");
  bad_engine.chunk_size  = 256;
  bad_engine.gpu_stride  = 256;
  bad_engine.engine      = "invalid_engine";
  EXPECT_THROW(GuiController{bad_engine}, std::invalid_argument);
}

TEST_F(GuiControllerLifecycle, RequestStopAfterFoundPreservesFoundState) {
  TempDir temp_dir;
  CliOptions opts = MakeTinyOptions(temp_dir.path(), target_addr_abcd_);
  GuiController controller(opts);
  ASSERT_TRUE(controller.start());
  controller.wait();
  ASSERT_EQ(controller.getStats().state, GuiState::Found);

  // Calling requestStop() on an already-finished Found run is a no-op and
  // must not overwrite GuiState::Found with GuiState::Stopped.
  controller.requestStop();
  EXPECT_EQ(controller.getStats().state, GuiState::Found);
}

// ============================================================================
// Suite 2: GuiControllerStats (2 tests)
// ============================================================================

TEST_F(GuiControllerStats, StatsPopulateDuringRun) {
  TempDir temp_dir;
  CliOptions opts;
  opts.range_start = RangeKey::FromHex("100000");
  opts.range_end = RangeKey::FromHex("2FFFFF");
  opts.chunk_size = 65536;
  opts.gpu_stride = 65536;
  opts.seed = 1;
  opts.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  opts.engine = "cpu";
  opts.output_dir = temp_dir.path();

  GuiController controller(opts);
  ASSERT_TRUE(controller.start());

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
  GuiStats stats = controller.getStats();
  while (stats.chunks_scanned < 2ULL &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stats = controller.getStats();
  }

  EXPECT_GE(stats.chunks_scanned, 2ULL);
  EXPECT_GT(stats.keys_scanned, 0ULL);
  EXPECT_GT(stats.elapsed_seconds, 0.0);
  EXPECT_GT(stats.rate_keys_per_sec, 0.0);

  controller.requestStop();
  controller.wait();

  GuiStats final_stats = controller.getStats();
  EXPECT_EQ(final_stats.state, GuiState::Stopped);
}

TEST_F(GuiControllerStats, GetStatsIsThreadSafe) {
  TempDir temp_dir;
  CliOptions opts;
  opts.range_start = RangeKey::FromHex("100000");
  opts.range_end = RangeKey::FromHex("8FFFFF");
  opts.chunk_size = 65536;
  opts.gpu_stride = 65536;
  opts.seed = 1;
  opts.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  opts.engine = "cpu";
  opts.output_dir = temp_dir.path();

  GuiController controller(opts);
  ASSERT_TRUE(controller.start());

  std::atomic<bool> saw_invalid_state{false};
  std::vector<std::thread> readers;
  readers.reserve(8);
  for (int t = 0; t < 8; ++t) {
    readers.emplace_back([&controller, &saw_invalid_state]() {
      for (int i = 0; i < 1000; ++i) {
        GuiStats s = controller.getStats();
        const bool valid = (s.state == GuiState::Idle ||
                            s.state == GuiState::Running ||
                            s.state == GuiState::Stopped ||
                            s.state == GuiState::Found ||
                            s.state == GuiState::Exhausted);
        if (!valid) {
          saw_invalid_state.store(true, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& th : readers) {
    th.join();
  }

  controller.requestStop();
  controller.wait();

  EXPECT_FALSE(saw_invalid_state.load());
}

// ============================================================================
// Suite 3: GuiControllerDestructor (2 tests)
// ============================================================================

TEST_F(GuiControllerDestructor, DestructorStopsRunningWorker) {
  TempDir temp_dir;
  CliOptions opts;
  opts.range_start = RangeKey::FromHex("100000");
  opts.range_end = RangeKey::FromHex("8FFFFF");
  opts.chunk_size = 524288;
  opts.gpu_stride = 524288;
  opts.seed = 1;
  opts.address = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
  opts.engine = "cpu";
  opts.output_dir = temp_dir.path();

  const auto start_time = std::chrono::steady_clock::now();
  {
    GuiController controller(opts);
    ASSERT_TRUE(controller.start());
  }
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start_time)
          .count();

  EXPECT_LT(elapsed_ms, 5000);
}

TEST_F(GuiControllerDestructor, DestructorOnIdleIsNoOp) {
  TempDir temp_dir;
  CliOptions opts = MakeTinyOptions(temp_dir.path(), target_addr_abcd_);

  {
    GuiController controller(opts);
    EXPECT_EQ(controller.getStats().state, GuiState::Idle);
  }
}

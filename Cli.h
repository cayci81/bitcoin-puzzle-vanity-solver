#ifndef PUZZLE71_CLI_H_
#define PUZZLE71_CLI_H_

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Engine.h"
#include "FoundMatchWriter.h"
#include "JsonRangeLogger.h"
#include "PrefixFilter.h"
#include "PuzzleSolver.h"
#include "RandomChunkGenerator.h"
#include "RangeKey.h"
#include "SECP256K1.h"

namespace puzzle::cli {

struct CliOptions {
  std::string address;                    // full target address (optional if prefix_b58 set)
  std::vector<std::string> address_list;  // multiple target addresses (e.g. from GUI file load)
  std::vector<std::string> prefix_b58;    // repeatable --prefix-b58 values
  std::string prefix_hex_csv;             // comma-separated hex prefixes for HexPrefixFilter
  std::filesystem::path address_file;     // --address-file: one full P2PKH address per line
  std::filesystem::path prefix_file;      // --prefix-file: one hex or Base58 prefix per line
  RangeKey range_start;
  RangeKey range_end;
  uint64_t chunk_size = 100'000'000;
  uint64_t gpu_stride = 262144;
  uint32_t max_collision_retries = 8;
  uint64_t seed = 0;                      // 0 = derive from current time
  std::filesystem::path output_dir = ".";
  std::string engine = "cpu";             // "cpu" or "gpu"
  int gpu_id = 0;
  int gpu_grid_x = -1;                    // -1 means auto (SM_COUNT * 8)
  int gpu_grid_y = 128;                   // Threads per block
  uint64_t max_chunks = 0;                // 0 = unlimited
  int puzzle_id = 0;                      // --puzzle <id>: auto-load from puzzles.json
  bool use_endomorphism = false;          // reserved for v2
  std::function<void(const std::string& address)> on_prefix_found = nullptr;
};

struct ParsedArgs {
  CliOptions options;
  bool show_help = false;
  bool show_version = false;
};

struct RunResult {
  int exit_code = 0;
  SolverStats stats;
  SolverExitReason reason = SolverExitReason::Exhausted;
};

// Option A context-builder pattern: bundles all solver components and
// constructs PuzzleSolver in-place so both CLI (RunSolver) and GUI
// (GuiController) share identical initialization, gpu_table narrowing,
// and resume logic while allowing live 60fps access to solver->getStats().
// Non-copyable and non-movable because FoundMatchWriter and PuzzleSolver
// hold a pointer to `this->stop_flag`; C++17 guaranteed copy elision
// constructs SolverContext directly in target storage via BuildSolverContext.
struct SolverContext {
  CliOptions options;
  std::shared_ptr<Secp256K1> secp;
  std::shared_ptr<AddressMatcher> matcher;
  std::shared_ptr<RandomChunkGenerator> generator;
  std::shared_ptr<JsonRangeLogger> logger;
  std::shared_ptr<FoundMatchWriter> writer;
  std::shared_ptr<IEngine> engine;
  std::vector<uint8_t> gpu_table;
  bool warn_dense_b58_only = false;
  std::atomic<bool> stop_flag{false};
  std::unique_ptr<PuzzleSolver> solver;

  explicit SolverContext(const CliOptions& opts);
  SolverContext(const SolverContext&) = delete;
  SolverContext& operator=(const SolverContext&) = delete;
};

SolverContext BuildSolverContext(const CliOptions& options);

RunResult RunContext(SolverContext& ctx,
                     std::atomic<bool>* external_stop = nullptr);

ParsedArgs ParseArgs(int argc, char** argv);

void LoadConfigFile(const std::filesystem::path& config_path,
                    CliOptions& options,
                    bool* has_range_out = nullptr);

RunResult RunSolver(const CliOptions& options,
                    std::atomic<bool>* external_stop = nullptr);

void PrintHelp();
void PrintVersion();

}  // namespace puzzle::cli

#endif  // PUZZLE71_CLI_H_

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include "Engine.h"
#include "SECP256k1.h"

namespace puzzle {

struct GpuEngineConfig {
  int gpu_id = 0;
  // VanitySearch defaults (GPUEngine.cu:206-210):
  //   grid_size_x = -1  => 8 * deviceProp.multiProcessorCount blocks
  //   grid_size_y = 128 => 128 threads per block
  //   nbThread    = grid_size_x * grid_size_y
  //   max_found   = 65536 ITEMs per kernel launch
  int grid_size_x = -1;
  int grid_size_y = 128;
  uint32_t max_found = 65536;
};

struct GpuEngineHooks {
  // Called after each kernel launch step completes and its ITEMs are harvested,
  // with (steps_done, keys_done). Tests use it to raise stop_flag deterministically
  // at a step boundary.
  std::function<void(uint64_t steps_done, uint64_t keys_done)> on_step = nullptr;
};

// Forward declarations for CUDA-specific types (to keep the header CPU-clean)
struct GpuEngineImpl;

class GpuEngine : public IEngine {
 public:
  // Matches GRP_SIZE (GPU/GPUGroup.h:3) and STEP_SIZE (GPU/GPUEngine.h:31).
  static constexpr int kGroupSize = 1024;
  static constexpr int kStepSize = 1024;
  static constexpr uint32_t kDefaultMaxFound = 65536;
  static constexpr size_t kGpuTableSize = 65536;
  static constexpr size_t kMaxGpuTablePopcount = 64;

  static size_t countActivePrefixes(const std::vector<uint8_t>& gpu_table);
  static void validateGpuTable(const std::vector<uint8_t>& gpu_table);

  // Throws std::runtime_error if dense table popcount > 64.
  explicit GpuEngine(std::shared_ptr<Secp256K1> secp,
                     const std::vector<uint8_t>& gpu_table,
                     int gpu_id);

  explicit GpuEngine(std::shared_ptr<Secp256K1> secp,
                     const std::vector<uint8_t>& gpu_table,
                     GpuEngineConfig config = {},
                     GpuEngineHooks hooks = {});

  ~GpuEngine() override;

  EngineBatchResult scan(const Chunk& chunk,
                         const std::vector<uint8_t>& gpu_table,
                         std::atomic<bool>& stop_flag) override;

  // Live progress (Phase 3.5 contract)
  bool reportsLiveProgress() const noexcept override { return true; }
  void setProgressCounter(std::atomic<uint64_t>* counter) override;

  static bool isAvailable();

  int nbThread() const noexcept { return nb_thread_; }

 private:
  std::shared_ptr<Secp256K1> secp_;
  std::vector<uint8_t> gpu_table_;
  int gpu_id_ = 0;
  GpuEngineConfig config_;
  GpuEngineHooks hooks_;
  int nb_thread_ = 0;
  std::unique_ptr<GpuEngineImpl> impl_;  // Pimpl to hide CUDA headers
  std::atomic<std::atomic<uint64_t>*> progress_{nullptr};
};

}  // namespace puzzle

#include "GpuEngine.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <mutex>

#ifdef PUZZLE71_ENABLE_CUDA
#include <cuda_runtime.h>
#include "GPU/GPUEngine.h"
#include "IntGroup.h"
#endif

namespace puzzle {

#ifdef PUZZLE71_ENABLE_CUDA
struct GpuEngineImpl {
  std::shared_ptr<Secp256K1> secp;
  std::unique_ptr<::GPUEngine> gpu;
  std::vector<::Point> start_points;  // Pre-allocated (size = nbThread) for SetKeys
  std::vector<::Point> step_gn;       // (i + 1) * (kStepSize * G) for i in [0, kGroupSize/2)
  ::Point step_2gn;                   // kGroupSize * (kStepSize * G)
  int last_step_threads = 1;          // Tracks current _2Gnx/_2Gny step multiplier on device
  std::mutex gpu_mutex;               // Protect GPUEngine state during scan

  GpuEngineImpl(std::shared_ptr<Secp256K1> secp_in,
                const std::vector<uint8_t>& gpu_table,
                const GpuEngineConfig& config)
      : secp(std::move(secp_in)),
        step_gn(GpuEngine::kGroupSize / 2) {
    // 1. Initialize GPUEngine with rekey=true to keep pinned memory alive.
    gpu = std::make_unique<::GPUEngine>(
        config.grid_size_x, config.grid_size_y, config.gpu_id, config.max_found,
        /*rekey=*/true);

    const int nb_thread = gpu->GetNbThread();
    if (!gpu->IsInitialised() || nb_thread <= 0) {
      throw std::runtime_error("CUDA OOM or init failed");
    }

    // Initialize all nb_thread slots with a valid curve point so idle threads
    // (when active_threads < nb_thread on small chunks) have valid coordinates.
    Int idle_key;
    idle_key.SetInt32(GpuEngine::kGroupSize / 2 + 1);
    ::Point idle_point = secp->ComputePublicKey(&idle_key);
    start_points.assign(static_cast<size_t>(nb_thread), idle_point);

    // Precompute multiples of G_step = kStepSize * G for fast grouped thread
    // seeding via IntGroup::ModInv.
    Int step_scalar;
    step_scalar.SetInt32(GpuEngine::kStepSize);
    ::Point g_step = secp->ComputePublicKey(&step_scalar);
    step_gn[0] = g_step;
    ::Point g = secp->DoubleDirect(g_step);
    step_gn[1] = g;
    for (int i = 2; i < GpuEngine::kGroupSize / 2; ++i) {
      g = secp->AddDirect(g, g_step);
      step_gn[i] = g;
    }
    step_2gn = secp->DoubleDirect(step_gn[GpuEngine::kGroupSize / 2 - 1]);

    // 2. Set Search Mode & Type
    gpu->SetSearchMode(SEARCH_COMPRESSED);
    gpu->SetSearchType(P2PKH);

    // 3. Upload the prefix table
    std::vector<prefix_t> active_prefixes;
    for (size_t i = 0; i < gpu_table.size(); ++i) {
      if (gpu_table[i] != 0) {
        active_prefixes.push_back(static_cast<prefix_t>(i));
      }
    }
    gpu->SetPrefix(active_prefixes);
  }

  // Populates start_points[0 .. active_threads - 1] with contiguous thread windows:
  //   start_points[t] = (base_key + kGroupSize/2 + t * kStepSize) * G
  void seedThreadPoints(const Int& base_key, int active_threads) {
    constexpr int kGrp = GpuEngine::kGroupSize;
    constexpr int kHalf = kGrp / 2;
    constexpr int kHLength = kHalf - 1;
    const uint64_t thread_stride_keys = GpuEngine::kStepSize;

    auto& gn = step_gn;
    ::Point& two_gn = step_2gn;
    int base_t = 0;
    if (active_threads >= kGrp) {
      Int center_key(const_cast<Int*>(&base_key));
      center_key.Add(static_cast<uint64_t>(kHalf) * thread_stride_keys +
                     static_cast<uint64_t>(kHalf));
      ::Point startP = secp->ComputePublicKey(&center_key);

      IntGroup grp(kHalf + 1);
      std::vector<Int> dx(kHalf + 1);
      grp.Set(dx.data());

      Int dy;
      Int dyn;
      Int _s;
      Int _p;

      for (; base_t + kGrp <= active_threads; base_t += kGrp) {
        int i = 0;
        for (; i < kHLength; ++i) {
          dx[i].ModSub(&gn[i].x, &startP.x);
        }
        dx[i].ModSub(&gn[i].x, &startP.x);
        dx[i + 1].ModSub(&two_gn.x, &startP.x);

        const bool first_point_at_infinity =
            dx[i].IsZero() && startP.y.IsEqual(&gn[i].y);
        for (int k = 0; k <= kHalf; ++k) {
          if (dx[k].IsZero()) {
            dx[k].SetInt32(1);
          }
        }

        grp.ModInv();

        start_points[base_t + kHalf] = startP;

        for (i = 0; i < kHLength; ++i) {
          ::Point pp = startP;
          ::Point pn = startP;

          dy.ModSub(&gn[i].y, &pp.y);
          _s.ModMulK1(&dy, &dx[i]);
          _p.ModSquareK1(&_s);
          pp.x.ModNeg();
          pp.x.ModAdd(&_p);
          pp.x.ModSub(&gn[i].x);
          pp.y.ModSub(&gn[i].x, &pp.x);
          pp.y.ModMulK1(&_s);
          pp.y.ModSub(&gn[i].y);

          dyn.Set(&gn[i].y);
          dyn.ModNeg();
          dyn.ModSub(&pn.y);
          _s.ModMulK1(&dyn, &dx[i]);
          _p.ModSquareK1(&_s);
          pn.x.ModNeg();
          pn.x.ModAdd(&_p);
          pn.x.ModSub(&gn[i].x);
          pn.y.ModSub(&gn[i].x, &pn.x);
          pn.y.ModMulK1(&_s);
          pn.y.ModAdd(&gn[i].y);

          start_points[base_t + kHalf + (i + 1)] = pp;
          start_points[base_t + kHalf - (i + 1)] = pn;
        }

        if (first_point_at_infinity) {
          start_points[base_t].Clear();
        } else {
          ::Point pn = startP;
          dyn.Set(&gn[i].y);
          dyn.ModNeg();
          dyn.ModSub(&pn.y);
          _s.ModMulK1(&dyn, &dx[i]);
          _p.ModSquareK1(&_s);
          pn.x.ModNeg();
          pn.x.ModAdd(&_p);
          pn.x.ModSub(&gn[i].x);
          pn.y.ModSub(&gn[i].x, &pn.x);
          pn.y.ModMulK1(&_s);
          pn.y.ModAdd(&gn[i].y);
          start_points[base_t] = pn;
        }

        if (base_t + 2 * kGrp <= active_threads) {
          ::Point pp = startP;
          dy.ModSub(&two_gn.y, &pp.y);
          _s.ModMulK1(&dy, &dx[i + 1]);
          _p.ModSquareK1(&_s);
          pp.x.ModNeg();
          pp.x.ModAdd(&_p);
          pp.x.ModSub(&two_gn.x);
          pp.y.ModSub(&two_gn.x, &pp.x);
          pp.y.ModMulK1(&_s);
          pp.y.ModSub(&two_gn.y);
          startP = pp;
        }
      }
    }

    for (int t = base_t; t < active_threads; ++t) {
      Int center_key(const_cast<Int*>(&base_key));
      center_key.Add(static_cast<uint64_t>(t) * thread_stride_keys +
                     static_cast<uint64_t>(kHalf));
      start_points[t] = secp->ComputePublicKey(&center_key);
    }
  }
};
#else
struct GpuEngineImpl {};
#endif

size_t GpuEngine::countActivePrefixes(const std::vector<uint8_t>& gpu_table) {
  size_t count = 0;
  for (uint8_t v : gpu_table) {
    if (v != 0) {
      ++count;
    }
  }
  return count;
}

void GpuEngine::validateGpuTable(const std::vector<uint8_t>& table) {
  if (table.size() != kGpuTableSize) {
    throw std::runtime_error("GpuEngine requires 65536-byte gpu_table");
  }
  if (countActivePrefixes(table) > kMaxGpuTablePopcount) {
    throw std::runtime_error(
        "GpuEngine requires gpu_table popcount <= 64 to prevent maxFound overflow; "
        "use CpuEngine for dense filters.");
  }
}

GpuEngine::GpuEngine(std::shared_ptr<Secp256K1> secp,
                     const std::vector<uint8_t>& gpu_table,
                     int gpu_id)
    : GpuEngine(std::move(secp),
                gpu_table,
                GpuEngineConfig{gpu_id, -1, 128, kDefaultMaxFound},
                GpuEngineHooks{}) {}

GpuEngine::GpuEngine(std::shared_ptr<Secp256K1> secp,
                     const std::vector<uint8_t>& gpu_table,
                     GpuEngineConfig config,
                     GpuEngineHooks hooks)
    : secp_(std::move(secp)),
      gpu_table_(gpu_table),
      gpu_id_(config.gpu_id),
      config_(config),
      hooks_(std::move(hooks)) {
  if (!secp_) {
    throw std::invalid_argument(
        "GpuEngine requires a non-null Secp256K1 instance.");
  }
  validateGpuTable(gpu_table_);

#ifdef PUZZLE71_ENABLE_CUDA
  impl_ = std::make_unique<GpuEngineImpl>(secp_, gpu_table_, config_);
  nb_thread_ = impl_->gpu->GetNbThread();
#endif
}

GpuEngine::~GpuEngine() = default;

bool GpuEngine::isAvailable() {
#ifdef PUZZLE71_ENABLE_CUDA
  int count = 0;
  cudaError_t err = cudaGetDeviceCount(&count);
  return (err == cudaSuccess) && (count > 0);
#else
  return false;
#endif
}

void GpuEngine::setProgressCounter(std::atomic<uint64_t>* counter) {
  progress_.store(counter, std::memory_order_release);
}

EngineBatchResult GpuEngine::scan(const Chunk& chunk,
                                  const std::vector<uint8_t>& gpu_table,
                                  std::atomic<bool>& stop_flag) {
  validateGpuTable(gpu_table);
#ifndef PUZZLE71_ENABLE_CUDA
  (void)chunk;
  (void)stop_flag;
  throw std::runtime_error("GPU engine not compiled in");
#else
  EngineBatchResult out;
  if (!impl_) {
    throw std::runtime_error("GpuEngineImpl not initialized");
  }

  std::lock_guard<std::mutex> lock(impl_->gpu_mutex);

  if (stop_flag.load(std::memory_order_acquire)) {
    out.aborted_early = true;
    out.keys_scanned = 0;
    return out;
  }

  // chunk.scanned_keys is the total keys to process.
  const uint64_t total_keys = chunk.scanned_keys;
  if (total_keys == 0) {
    return out;
  }

  const Int chunk_base_key = chunk.start.value;

  const int total_gpu_threads = impl_->gpu->GetNbThread();
  int active_threads = std::min(
      total_gpu_threads,
      static_cast<int>((total_keys + kStepSize - 1) / kStepSize));
  if (active_threads <= 0) {
    active_threads = 1;
  }

  const uint64_t step_capacity =
      static_cast<uint64_t>(active_threads) * kStepSize;
  const uint64_t num_steps = (total_keys + step_capacity - 1) / step_capacity;

  // 1. Ensure device constant _2Gnx/_2Gny advances each thread by step_capacity * G
  //    between steps so that step `step` scans the contiguous slice
  //    [chunk.start + step * step_capacity, chunk.start + (step + 1) * step_capacity).
  if (num_steps > 1 && active_threads != impl_->last_step_threads) {
    Int step_scalar;
    step_scalar.SetInt32(0);
    step_scalar.Add(step_capacity);
    ::Point step_pt = secp_->ComputePublicKey(&step_scalar);
    if (!impl_->gpu->SetStepPoint(step_pt)) {
      throw std::runtime_error("GPUEngine::SetStepPoint failed");
    }
    impl_->last_step_threads = active_threads;
  }

  // 2. Seed all active threads ONCE for step 0:
  //    thread t starts at [chunk.start + t * kStepSize, chunk.start + (t + 1) * kStepSize).
  impl_->seedThreadPoints(chunk_base_key, active_threads);
  if (!impl_->gpu->SetKeys(impl_->start_points.data())) {
    throw std::runtime_error("GPUEngine::SetKeys failed");
  }

  uint64_t done = 0;
  for (uint64_t step = 0; step < num_steps; ++step) {
    if (stop_flag.load(std::memory_order_acquire)) {
      out.aborted_early = true;
      break;
    }

    const bool launch_next =
        (step + 1 < num_steps) && !stop_flag.load(std::memory_order_acquire);

    // Harvest results of step `step` (and conditionally launch step `step + 1`).
    std::vector<::ITEM> found;
    if (!impl_->gpu->Launch(found, /*spinWait=*/true, launch_next)) {
      throw std::runtime_error("GPUEngine::Launch failed");
    }

    for (const auto& it : found) {
      if (it.endo != 0) {
        continue;  // Skip endomorphism multiples (Q7)
      }
      if (static_cast<int>(it.thId) >= active_threads) {
        continue;  // Ignore idle threads when active_threads < nbThread
      }

      const uint64_t offset =
          static_cast<uint64_t>(it.incr < 0 ? -it.incr : it.incr);
      if (offset >= static_cast<uint64_t>(kStepSize)) {
        continue;
      }

      const uint64_t chunk_offset =
          step * step_capacity +
          static_cast<uint64_t>(it.thId) * kStepSize +
          offset;
      if (chunk_offset >= total_keys) {
        continue;  // Beyond the end of the chunk
      }

      Int candidate(const_cast<Int*>(&chunk_base_key));
      candidate.Add(chunk_offset);

      RangeKey cand_rk(candidate);
      if (cand_rk < chunk.start || cand_rk > chunk.end) {
        continue;
      }

      // Host-side verification: does the candidate's hash match the ITEM's hash?
      Point pub = secp_->ComputePublicKey(&candidate);
      uint8_t expected_hash[20];
      secp_->GetHash160(P2PKH, true, pub, expected_hash);

      if (std::memcmp(expected_hash, it.hash, 20) == 0) {
        std::string addr = secp_->GetAddress(P2PKH, true, expected_hash);
        std::array<uint8_t, 20> h160{};
        std::memcpy(h160.data(), expected_hash, 20);

        out.matched_hash160s.push_back(h160);
        out.matched_addresses.push_back(addr);
        out.matched_privkeys_hex.push_back(candidate.GetBase16());
      }
    }

    const uint64_t remaining = total_keys - done;
    const uint64_t keys_this_step = std::min(step_capacity, remaining);
    done += keys_this_step;

    std::atomic<uint64_t>* p = progress_.load(std::memory_order_acquire);
    if (p != nullptr) {
      p->fetch_add(keys_this_step, std::memory_order_relaxed);
    }

    if (hooks_.on_step) {
      hooks_.on_step(step + 1, done);
    }
  }

  out.keys_scanned = done;
  return out;
#endif
}

}  // namespace puzzle

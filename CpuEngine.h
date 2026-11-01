#pragma once

// CPU reference implementation of puzzle::IEngine, built directly on the
// VanitySearch secp256k1 CPU path. It exists for two reasons:
//   1. CI / non-CUDA machines can run the full coordinator (PuzzleSolver)
//      against a REAL engine instead of a mock.
//   2. It is the correctness oracle for the future GpuEngine: both implement
//      IEngine and must produce identical EngineBatchResult contents for the
//      same chunk + gpu_table.
//
// CHUNK -> SCAN MAPPING (mirrors VanitySearch::FindKeyCPU, Vanity.cpp:1284):
//   a) Seeding: the chunk's RangeKey start IS the private key (no seed/rng).
//      Like VanitySearch::getCPUStartingKey with rekey==0 (Vanity.cpp:1266),
//      the group center point is startP = secp->ComputePublicKey(key + GRP/2).
//   b) Increment: keys are scanned as one batch of kGroupSize consecutive
//      points per group modular inversion (IntGroup::ModInv), using the same
//      gn_[i] = (i+1)*G / g2n_ = kGroupSize*G tables Vanity.cpp:268-278 builds.
//      pts[i] == pubkey(key + i), so the private key of a hit at index i is
//      simply key + i (no inversion needed).
//   c) Prefix check: hash160 = secp->GetHash160(P2PKH, compressed, pts[i]);
//      idx = h[0] | (h[1] << 8) — identical to VanitySearch's
//      `prefix_t pr0 = *(prefix_t *)h0` (little-endian uint16, GPUEngine.h:38).
//      gpu_table IS the prefix table; VanitySearch's PREFIX_TABLE_ITEM
//      structure is bypassed because classification happens host-side in
//      AddressMatcher.
//   d) Output: address = secp->GetAddress(P2PKH, compressed, hash160)
//      (Base58Check, SECP256K1.cpp:710); private key hex = Int::GetBase16()
//      (uppercase, no leading zeros — same format VanitySearch outputs in
//      checkPrivKey, Vanity.cpp:824).
//   e) stop_flag: polled at the top of every group AND after every key in the
//      hash loop (VanitySearch polls `endOfSearch` the same way). On abort:
//      aborted_early=true and keys_scanned == exact number of keys hashed.
//
// SCOPE (v1): compressed P2PKH only. No endomorphism/symmetry multiplication
// (VanitySearch's 6x trick). keys_scanned therefore counts PRIVATE KEYS, each
// producing exactly one address candidate. RangeRecord.endomorphism_covered
// stays false for chunks scanned by this engine.
//
// PUZZLE 71 COMPRESSED-KEY ASSUMPTION: This engine produces compressed
// P2PKH addresses only. Bitcoin puzzles (1-70, all solved) use compressed
// keys by convention, so this is the expected production setting. If a
// future puzzle requires uncompressed P2PKH, this engine and GpuEngine
// must be extended (VanitySearch's CPU path already supports both via
// its -c/-u flags). Do not silently change the 'true' argument without
// re-running the full puzzle test suite.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Engine.h"
#include "IntGroup.h"
#include "Point.h"
#include "SECP256k1.h"

namespace puzzle {

// Test hooks, following the FoundMatchWriter sync_hook/write_hook precedent.
// Production code leaves the defaults (all nullptr).
struct CpuEngineHooks {
  // Called after each key is hashed and checked, with the running count of
  // keys scanned in the current scan() call (first key -> 1). Tests use it to
  // raise stop_flag at an exact, deterministic key count.
  std::function<void(uint64_t keys_done)> on_key = nullptr;
};

class CpuEngine : public IEngine {
 public:
  // secp must already be Init()'ed by the caller; it is shared so that
  // multiple engines / solver threads reuse a single generator table.
  // CpuEngine never calls Secp256K1::Init() or mutates shared secp state
  // (all scan-time math uses local Int/Point temporaries).
  explicit CpuEngine(std::shared_ptr<Secp256K1> secp, CpuEngineHooks hooks = {});

  EngineBatchResult scan(const Chunk& chunk,
                         const std::vector<uint8_t>& gpu_table,
                         std::atomic<bool>& stop_flag) override;

  bool reportsLiveProgress() const noexcept override { return true; }
  void setProgressCounter(std::atomic<uint64_t>* counter) override;

  // Group size: identical to VanitySearch's CPU_GRP_SIZE (Vanity.h:26).
  // Re-declared here so CpuEngine does not include Vanity.h (which drags in
  // the CUDA GPUEngine header).
  static constexpr int kGroupSize = 1024;

 private:
  std::shared_ptr<Secp256K1> secp_;
  CpuEngineHooks hooks_;
  std::atomic<std::atomic<uint64_t>*> progress_{nullptr};

  // Generator tables, same layout as Vanity.cpp:36-37:
  //   gn_[i] = (i+1)*G   for i in [0, kGroupSize/2)
  //   g2n_   = kGroupSize*G  (== 2 * gn_[kGroupSize/2 - 1])
  // Built once in the constructor; scanning is read-only afterwards.
  Point gn_[kGroupSize / 2];
  Point g2n_;
  std::vector<Point> pts_;
};

}  // namespace puzzle

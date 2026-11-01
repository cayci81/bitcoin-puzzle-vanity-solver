#include "CpuEngine.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace puzzle {

CpuEngine::CpuEngine(std::shared_ptr<Secp256K1> secp, CpuEngineHooks hooks)
    : secp_(std::move(secp)), hooks_(std::move(hooks)), pts_(kGroupSize) {
  // Generator table, identical construction to Vanity.cpp:268-278:
  //   gn_[i] = (i+1)*G, g2n_ = kGroupSize*G.
  Point g = secp_->G;
  gn_[0] = g;
  g = secp_->DoubleDirect(g);
  gn_[1] = g;
  for (int i = 2; i < kGroupSize / 2; i++) {
    g = secp_->AddDirect(g, secp_->G);
    gn_[i] = g;
  }
  g2n_ = secp_->DoubleDirect(gn_[kGroupSize / 2 - 1]);
}

void CpuEngine::setProgressCounter(std::atomic<uint64_t>* counter) {
  progress_.store(counter, std::memory_order_release);
}

EngineBatchResult CpuEngine::scan(const Chunk& chunk,
                                  const std::vector<uint8_t>& gpu_table,
                                  std::atomic<bool>& stop_flag) {
  EngineBatchResult out;

  // (a) SEEDING: the chunk start IS the first private key (arbitrary 256-bit
  // start key; no seed/RNG involved). Same as getCPUStartingKey with rekey==0.
  Int key;
  key.Set(const_cast<Int*>(&chunk.start.value));

  const uint64_t total = chunk.scanned_keys;
  uint64_t done = 0;

  IntGroup grp(kGroupSize / 2 + 1);
  Int dx[kGroupSize / 2 + 1];
  auto& pts = pts_;
  Int dy;
  Int dyn;
  Int _s;
  Int _p;
  grp.Set(dx);

  while (done < total) {
    // (e) stop poll, group granularity (outer loop), like `!endOfSearch`.
    if (stop_flag.load(std::memory_order_acquire)) {
      out.aborted_early = true;
      break;
    }

    // Group center: startP = pubkey(key + kGroupSize/2).
    Int km(&key);
    km.Add((uint64_t)(kGroupSize / 2));
    Point startP = secp_->ComputePublicKey(&km);

    // (b) One batch of kGroupSize consecutive points per group ModInv.
    // This is the FindKeyCPU group fill (Vanity.cpp:1322-1414), kept
    // line-for-line so the two implementations cannot silently diverge.
    const int hLength = kGroupSize / 2 - 1;
    int i;
    for (i = 0; i < hLength; i++) {
      dx[i].ModSub(&gn_[i].x, &startP.x);
    }
    dx[i].ModSub(&gn_[i].x, &startP.x);    // For the first point
    dx[i + 1].ModSub(&g2n_.x, &startP.x);  // For the next center point

    // Guard Montgomery batch inversion (IntGroup::ModInv) against zero dx
    // entries on boundary keys (caught by CpuEngineTest.ChunkStartingAtKeyZero):
    //   - key == 0   => startP = 512*G == gn_[511], so dx[511] == 0
    //                   (pts[0] is 0*G, the point at infinity).
    //   - key == 512 => startP = 1024*G == g2n_,   so dx[512] == 0.
    // A single zero in dx[] zeroes the prefix product in IntGroup::ModInv
    // and corrupts all 513 inverses in the group. Substituting 1
    // (multiplicative identity) preserves all other inverses.
    const bool first_point_at_infinity = dx[i].IsZero();
    if (first_point_at_infinity) {
      dx[i].SetInt32(1);
    }
    if (dx[i + 1].IsZero()) {
      dx[i + 1].SetInt32(1);
    }

    grp.ModInv();

    // pts[kGroupSize/2 + j] = startP + j*G, pts[kGroupSize/2 - j] = startP - j*G
    // => pts[i] == pubkey(key + i) for i in [0, kGroupSize).
    pts[kGroupSize / 2] = startP;

    for (i = 0; i < hLength; i++) {
      Point pp = startP;
      Point pn = startP;

      // P = startP + (i+1)*G
      dy.ModSub(&gn_[i].y, &pp.y);
      _s.ModMulK1(&dy, &dx[i]);
      _p.ModSquareK1(&_s);
      pp.x.ModNeg();
      pp.x.ModAdd(&_p);
      pp.x.ModSub(&gn_[i].x);
      pp.y.ModSub(&gn_[i].x, &pp.x);
      pp.y.ModMulK1(&_s);
      pp.y.ModSub(&gn_[i].y);

      // P = startP - (i+1)*G  (if (x,y) = k*G then (x,-y) = -k*G)
      dyn.Set(&gn_[i].y);
      dyn.ModNeg();
      dyn.ModSub(&pn.y);
      _s.ModMulK1(&dyn, &dx[i]);
      _p.ModSquareK1(&_s);
      pn.x.ModNeg();
      pn.x.ModAdd(&_p);
      pn.x.ModSub(&gn_[i].x);
      pn.y.ModSub(&gn_[i].x, &pn.x);
      pn.y.ModMulK1(&_s);
      pn.y.ModAdd(&gn_[i].y);

      pts[kGroupSize / 2 + (i + 1)] = pp;
      pts[kGroupSize / 2 - (i + 1)] = pn;
    }

    // First point (startP - kGroupSize/2*G) == pubkey(key).
    if (first_point_at_infinity) {
      pts[0].Clear();
    } else {
      Point pn = startP;
      dyn.Set(&gn_[i].y);
      dyn.ModNeg();
      dyn.ModSub(&pn.y);
      _s.ModMulK1(&dyn, &dx[i]);
      _p.ModSquareK1(&_s);
      pn.x.ModNeg();
      pn.x.ModAdd(&_p);
      pn.x.ModSub(&gn_[i].x);
      pn.y.ModSub(&gn_[i].x, &pn.x);
      pn.y.ModMulK1(&_s);
      pn.y.ModAdd(&gn_[i].y);
      pts[0] = pn;
    }

    // (c)/(d) Per-key: hash160 -> prefix16 -> gpu_table -> record match.
    // A partial final group computes all points but only checks the
    // remaining keys of the chunk.
    const uint64_t group_keys =
        std::min<uint64_t>(kGroupSize, total - done);
    for (uint64_t j = 0; j < group_keys; j++) {
      unsigned char h[20];
      secp_->GetHash160(P2PKH, true, pts[j], h);
      const uint16_t idx = static_cast<uint16_t>(h[0] | (h[1] << 8));

      if (idx < gpu_table.size() && gpu_table[idx] != 0) {
        // Private key of pts[j] is key + j (points are consecutive).
        Int k(&key);
        k.Add(j);
        std::array<uint8_t, 20> h160{};
        std::memcpy(h160.data(), h, 20);
        out.matched_hash160s.push_back(h160);
        out.matched_addresses.push_back(secp_->GetAddress(P2PKH, true, h));
        out.matched_privkeys_hex.push_back(k.GetBase16());
      }

      done++;
      if (hooks_.on_key) {
        hooks_.on_key(done);
      }
      std::atomic<uint64_t>* p = progress_.load(std::memory_order_acquire);
      if (p) {
        p->fetch_add(1, std::memory_order_relaxed);
      }
      // (e) stop poll, key granularity: abort with the exact count.
      if (stop_flag.load(std::memory_order_acquire)) {
        out.aborted_early = true;
        out.keys_scanned = done;
        // Leave progress_ as-is. The caller is responsible for stopping the
        // counter wiring after scan() returns.
        return out;
      }
    }

    // Advance to the next group of consecutive keys.
    key.Add((uint64_t)kGroupSize);
  }

  out.keys_scanned = done;
  return out;
}

}  // namespace puzzle

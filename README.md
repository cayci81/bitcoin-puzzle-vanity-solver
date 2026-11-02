# Bitcoin Puzzle #71 Solver

An educational, non-commercial C++17 / CUDA implementation of a partitioned compressed-P2PKH keyspace scanner modeled on the Bitcoin Puzzle #71 challenge (`[2^70, 2^71 - 1]`, target address `1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU`).

It includes:
- **Deterministic & Random Stride-Aligned Chunk Partitioning** (`RangeKey`, `RandomChunkGenerator`) with gap-fallback when random sampling collides.
- **Crash-Safe Resume Logging** (`JsonRangeLogger`) that records completed and partial/aborted chunks to `scanned_ranges.jsonl` (with log rotation) and replays them on startup.
- **Two-Stage Prefix & Full-Address Filtering** (`HexPrefixFilter`, `AddressMatcher`) with automatic 16-bit `hash160` prefix narrowing for full target addresses.
- **Durable Match Persistence** (`FoundMatchWriter`) that writes matches to `found_matches.jsonl` and flushes to disk with `_commit` / `fsync`.
- **Pluggable Compute Engines** (`CpuEngine` and CUDA `GpuEngine` adapting VanitySearch's `GPUEngine` with zero `.cu` kernel diff and host-side endomorphism/symmetry filtering).
- **Multi-Target Vanity & Batch Matching** (`TargetListLoader`, `HexPrefixFilter`, `AddressMatcher`) supporting hundreds of target addresses (`--address-file`) and vanity prefixes (`--prefix-file`).
- **Human-Readable Match Outputs** (`prefix_found.txt` and `found_matches.jsonl`) recording vanity matches with timestamp, matched prefix, address, and private key.
- **Crypto Intelligence & Caching** (`NetworkAPI`, `PuzzleDatabase`) with asynchronous balance and transaction count queries via Blockchain.info API, protected by in-memory caching and 1-second rate-limiting.
- **Search Statistics & Progress Telemetry** (`PuzzleSolver`, `GuiController`) computing 256-bit total keyspace size, scanned key counter, and exact percentage progress.
- **Turkish Localization & Multi-Address GUI File Loading** (`puzzle71_gui`, `puzzle71_solver`) supporting single or batch address file loading directly from Windows file dialogs, with fully localized Turkish interface and help texts.

---

## Performance Reality

| Backend | Hardware | Measured Primary Key Rate | Raw 6x EC+Hash160 Kernel Rate |
| :--- | :--- | :--- | :--- |
| `CpuEngine` (Debug) | Single CPU thread | ~355K keys/s | ~355K evals/s |
| `GpuEngine` (Debug, unoptimized host seed) | NVIDIA RTX 4060 (`sm_89`) | 74.2M keys/s | 445.2M evals/s |
| `GpuEngine` (Release, `IntGroup` seed + chained launch) | NVIDIA RTX 4060 (`sm_89`) | **275.1M keys/s** (`275,100,000` keys/s) | **1.65B evals/s** (`1,650.6M` evals/s) |

> **Why primary key rate vs. raw VanitySearch rate differs by 6x:** VanitySearch's `ComputeKeysComp` kernel computes 6 compressed `hash160` values per elliptic-curve point (3 secp256k1 endomorphisms $\times$ 2 $y$-parities). Because v1 scans contiguous `[chunk.start, chunk.end]` ranges without modifying the `.cu` kernel, `GpuEngine` filters for primary in-range keys (`endo == 0` and true $y$-parity), achieving **275.1M primary keys/s** (**1.65 GH/s** raw kernel throughput) on a single RTX 4060.
>
> **Scale reality:** Puzzle #71 spans $2^{70} \approx 1.18 \times 10^{21}$ keys. Even at 1.5 GH/s, exhaustively scanning $2^{70}$ keys takes **~15,000–25,000 years** on a single consumer GPU. This tool is designed for learning the architecture and engineering mechanics of high-throughput secp256k1 key search, not for solving Puzzle #71 on a single desktop GPU.

---

## Build Instructions

### Prerequisites
- CMake 3.20+
- C++17 compiler (MSVC 2022 on Windows, GCC/Clang on Linux)
- *(Optional, for GPU)* CUDA Toolkit 12.0+ (tested with CUDA 12.8 and 13.3 targeting `sm_89`)

### 1. CPU-Only Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### 2. CUDA GPU Build (`PUZZLE71_BUILD_CUDA=ON`)
```bash
cmake -S . -B build-cuda -DPUZZLE71_BUILD_CUDA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-cuda --config Release
ctest --test-dir build-cuda -C Release --output-on-failure
```

*(Note: When configuring from the root `CMakeLists.txt`, executables are placed under `build/tests/Release/` or `build-cuda/tests/Release/` on multi-config generators like Visual Studio, or directly under `build/Release/` when configuring with `-S tests`.)*

---

## Run Instructions

### CLI (`puzzle71_solver`)

1. **Run by Puzzle ID (auto-loads range & address from `puzzles.json`):**
   ```bash
   ./build-cuda/Release/puzzle71_solver.exe --puzzle 71 --engine gpu
   ```

2. **Run using a TOML configuration file:**
   ```bash
   ./build-cuda/Release/puzzle71_solver.exe --config configs/puzzle71.toml
   ```

3. **Multi-Target Matching with address and prefix text files:**
   ```bash
   ./build-cuda/Release/puzzle71_solver.exe \
     --address-file targets.txt \
     --prefix-file prefixes.txt \
     --range-hex 400000000000000000:7fffffffffffffffff \
     --engine gpu
   ```

4. **Run a quick self-check on a known key (`0xABCD` -> `15QkBmpomYLhx71f2zKZpTt1eCXa4EEMya`):**
   ```bash
   ./build-cuda/Release/puzzle71_solver.exe \
     --address 15QkBmpomYLhx71f2zKZpTt1eCXa4EEMya \
     --range-hex AB00:ABFF \
     --chunk-size 256 \
     --gpu-stride 256 \
     --engine gpu \
     --output-dir ./demo_out
   ```

5. **Run a 100M-key GPU benchmark chunk:**
   ```bash
   ./build-cuda/Release/puzzle71_solver.exe \
     --address 15QkBmpomYLhx71f2zKZpTt1eCXa4EEMya \
     --range-hex 10000000:15FFFFF0 \
     --engine gpu \
     --chunk-size 100000000 \
     --gpu-stride 262144 \
     --max-chunks 1 \
     --output-dir ./gpu_bench
   ```

### GUI (`puzzle71_gui`, Windows DirectX 11)
```bash
./build-cuda/Release/puzzle71_gui.exe
```
- **Türkçe Arayüz & Kolay Kullanım**: Tüm etiketler, butonlar, durumlar ve telemetri paneli Türkçe olarak tasarlanmıştır.
- **Toplu Adres Yükleme (Adres Dosyası)**: "Adres Dosyası Yükle (.txt)" butonu ile Windows dosya seçici üzerinden yüzlerce hedef adresi içeren bir `.txt` dosyasını kolayca yükleyebilir ve tek seferde tarayabilirsiniz.
- **Detaylı Arama İstatistikleri**: Seçilen aralığın toplam anahtar alanı (256-bit tam hassasiyetle), taranan anahtar sayısı ve genel ilerleme yüzdesi (Progress Bar) gerçek zamanlı gösterilir.
- **Puzzle Önayarı (Preset)**: Açılır listeden Puzzle #1'den #160'a kadar tüm Bitcoin bulmacalarını seçebilirsiniz. Adres ve aralık otomatik doldurulur ve bakiye asenkron sorgulanır.
- **Adres İstihbaratı**: "Bakiye Sorgula" butonu ile adresin blockchain.info üzerindeki bakiyesi ve işlem geçmişi anında görüntülenir (`Yeni / Kullanılmamış`, `Kullanılmış - Boş`, ya da yeşil `BAKİYELİ! X BTC`).
- **Canlı Vanity Arama Paneli**: Keşfedilen vanity önekleri gerçek zamanlı olarak `prefix_found.txt` dosyasına yazılır ve GUI tablosunda işlem sayıları ile birlikte listelenir. Bakiyeli bir adres bulunursa anında alarm verir.
- **Hızlı Test**: "Demo Aralığı AB00:ABFF" butonuna tıklayarak bilinen `0xABCD` anahtarı üzerinde anında eşleşme testi yapabilirsiniz.
- **Motor Seçimi**: CPU veya GPU seçimi yaparak "Başlat" butonuna basın.

---

## Safety Note

Found private keys are written in JSON Lines format to `<output-dir>/found_matches.jsonl` and immediately flushed to disk via OS-level `fsync` (`_commit` on Windows). **Always back up `found_matches.jsonl` and `scanned_ranges.jsonl` to external storage before deleting build directories or closing your workspace.**

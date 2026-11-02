# Bitcoin Puzzle & Vanity Çözücü (Bitcoin Puzzle & Vanity Solver)

[![Platform: Windows](https://img.shields.io/badge/Platform-Windows%20x64-0078D6?logo=windows)](https://github.com/)
[![Language: C++17](https://img.shields.io/badge/Language-C%2B%2B17-00599C?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![CUDA: 12.0+](https://img.shields.io/badge/CUDA-12.0%2B%20%7C%20sm__89-76B900?logo=nvidia)](https://developer.nvidia.com/cuda-toolkit)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)
[![Tests: 221 Passing](https://img.shields.io/badge/Tests-221%20passing-brightgreen.svg)](tests/)

**Bitcoin Puzzle & Vanity Çözücü** is a high-performance, modular C++17 and CUDA cryptographic solver and vanity address search engine. Built specifically to explore the mathematical properties of secp256k1 elliptic curves, it features native hardware acceleration for modern NVIDIA GPUs (including Ada Lovelace RTX 40-series and Ampere), capable of generating and testing points at up to **1.65+ GH/s** raw throughput.

The project pairs a rugged, multithreaded command-line engine with a responsive DirectX 11 / Dear ImGui graphical dashboard. In addition to brute-force keyspace exploration, it incorporates a real-time cryptocurrency intelligence layer with live blockchain querying, transaction history tracking, and automated vanity prefix discovery.

![GUI Screenshot](docs/screenshot_gui.png)

---

## ✨ Features

- ⚡ **Native CUDA Acceleration (`sm_89` / Ada Lovelace & Ampere)**: Optimized GPU kernels using endomorphism techniques, achieving over 1.65 Billion elliptic curve point evaluations per second on consumer hardware like the NVIDIA RTX 4060.
- 🎯 **Multi-Target Batch Search**: Load `.txt` files containing up to 4,096 target Bitcoin addresses simultaneously on GPU (or unlimited on CPU) with zero algorithmic throughput degradation.
- 🌐 **Bilingual User Interface (English / Turkish)**: Seamless on-the-fly language switching for both GUI and CLI. Full Turkish localization with native TrueType Unicode font rendering (`Segoe UI` / `Tahoma`).
- 📊 **Real-Time Crypto Intelligence Dashboard**:
  - Live address balance checking and UTXO status via `blockchain.info`.
  - Transaction count (`n_tx`) analysis to determine if addresses were previously active.
  - Dedicated alerting system for newly found addresses holding confirmed balances.
  - In-memory API query cache with rate-limit protection.
- 🎛️ **Occupancy & Thread Tuning**: Fine-tune GPU execution grids (`--gpu-grid-x`, `--gpu-grid-y`) with automated one-click presets for modern GPUs.
- 🧪 **Exhaustive Test Suite**: 221 automated unit and regression tests verifying 256-bit multiprecision arithmetic, Base58Check encoding, Bloom filters, and background controllers.

---

## ⚡ Performance Reality & Educational Scope

While this engine achieves state-of-the-art throughput on consumer hardware, users should understand the astronomical scale of cryptographic keyspaces:

| Target / Puzzle | Range Span | GPU Speed (Raw) | Expected Completion Time |
| :--- | :--- | :--- | :--- |
| **Demo Range (`AB00:ABFF`)** | 256 keys | > 1.0 GH/s | < 0.001 seconds |
| **Puzzle #40** | $2^{40} \approx 1.09 \times 10^{12}$ | 1.65 GH/s | ~ 11 minutes |
| **Puzzle #50** | $2^{50} \approx 1.12 \times 10^{15}$ | 1.65 GH/s | ~ 8 days |
| **Puzzle #71** | $2^{70} \approx 1.18 \times 10^{21}$ | 1.65 GH/s | **~ 22,000 to 130,000 years** |

> [!NOTE]
> This tool demonstrates parallel compute architecture, GPU kernel design, and cryptographic mathematics. It is not an overnight mechanism to break 71-bit+ puzzles.

---

## 🛠️ Building from Source

### Prerequisites
- **Operating System**: Windows 10 / 11 (64-bit)
- **Compiler**: Visual Studio 2022 (MSVC v143+) with C++17
- **CMake**: Version 3.25 or newer
- **CUDA Toolkit (Optional for GPU mode)**: CUDA 12.0 or newer

### 1. Build CPU Mode (No CUDA required)
```bash
cmake -B build -DPUZZLE71_BUILD_CUDA=OFF -DPUZZLE71_BUILD_TESTS=ON -DPUZZLE71_BUILD_GUI=ON
cmake --build build --config Release
```

### 2. Build CUDA GPU Mode
```bash
cmake -B build-cuda -DPUZZLE71_BUILD_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES="89" -DPUZZLE71_BUILD_TESTS=ON -DPUZZLE71_BUILD_GUI=ON
cmake --build build-cuda --config Release
```

---

## 🚀 Usage

### Graphical Interface (GUI)
Launch the graphical dashboard:
```bash
.\build-cuda\Release\puzzle71_gui.exe
```
1. Select your puzzle preset (e.g. **Puzzle 71** or **Demo Range**).
2. Or load a target list of Bitcoin addresses via **"Adres Dosyası Yükle (.txt)"** / **"Load Address File (.txt)"**.
3. Choose the compute engine (**GPU** or **CPU**).
4. Click **"Başlat"** / **"Start"**.

### Command Line Interface (CLI)

#### 1. Search for a Bitcoin Puzzle target:
```bash
.\build-cuda\Release\puzzle71_solver.exe --puzzle 71 --engine gpu
```

#### 2. Search a custom hex range with multi-address list:
```bash
.\build-cuda\Release\puzzle71_solver.exe --range-hex 400000000000000000:7fffffffffffffffff --address-file targets.txt --engine gpu
```

#### 3. CLI Turkish Language Option:
```bash
.\build-cuda\Release\puzzle71_solver.exe --help --lang tr
```

#### 4. GPU Tuning Parameters:
```bash
.\build-cuda\Release\puzzle71_solver.exe --puzzle 71 --engine gpu --gpu-grid-x 192 --gpu-grid-y 256
```

---

## 🧪 Running Tests

Verify full system integrity with the built-in test suite:
```bash
.\build-cuda\Release\puzzle71_tests.exe
```
All 221 test suites must pass:
```text
[==========] 221 tests from 38 test suites ran.
[  PASSED  ] 221 tests.
```

---

## 📜 Legal & Ethical Disclaimer

> [!CAUTION]
> **This tool is developed strictly for educational, research, and cryptographic analysis purposes.**
> It is designed to evaluate wallet security, research elliptic curve mathematics, and benchmark modern GPU compute performance. The developers assume no liability for misuse, unauthorized access, or loss resulting from the use of this software. Always comply with the legal regulations of your jurisdiction.

---

## 📄 License

This project is licensed under the **GNU General Public License v3.0** (GPL-3.0). See the [LICENSE](LICENSE) file for complete details.

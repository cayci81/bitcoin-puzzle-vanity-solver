# Contributing to Bitcoin Puzzle & Vanity Çözücü (Solver)

Thank you for your interest in contributing to **Bitcoin Puzzle & Vanity Çözücü**!

## Code of Conduct
Please be respectful, constructive, and collaborative. Treat all community members with courtesy.

## Development Workflow

### 1. Requirements
- **OS**: Windows 10/11 (x64)
- **Compiler**: Visual Studio 2022 (MSVC v143+) with C++17 support
- **Build System**: CMake 3.25+
- **CUDA Toolkit** (Optional for GPU engine): CUDA 12.0+ (Compute Capability `sm_89` / Ada Lovelace recommended)

### 2. Building the Project

#### CPU Mode (No CUDA needed):
```bash
cmake -B build -DPUZZLE71_BUILD_CUDA=OFF -DPUZZLE71_BUILD_TESTS=ON -DPUZZLE71_BUILD_GUI=ON
cmake --build build --config Release
```

#### CUDA Mode:
```bash
cmake -B build-cuda -DPUZZLE71_BUILD_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES="89" -DPUZZLE71_BUILD_TESTS=ON -DPUZZLE71_BUILD_GUI=ON
cmake --build build-cuda --config Release
```

### 3. Running Unit Tests
All contributions must pass the test suite with 0 failures:
```bash
.\build\Release\puzzle71_tests.exe
# or for CUDA build:
.\build-cuda\Release\puzzle71_tests.exe
```

### 4. Guidelines & Conventions
- Maintain the modular clean architecture (`Matcher`, `Solver`, `NetworkAPI`, `GuiController`, `Localization`).
- Do not modify secp256k1 elliptic curve arithmetic unless fixing a proven mathematical bug.
- All new GUI text must be added to `Localization.cpp` in both Turkish (`TR`) and English (`EN`).
- Add comprehensive Google Tests for any newly introduced features or bugfixes.

### 5. Submitting Changes
1. Fork the repository and create your feature branch (`git checkout -b feature/my-enhancement`).
2. Commit your changes with concise and descriptive commit messages.
3. Push to your branch and open a Pull Request against `master`.

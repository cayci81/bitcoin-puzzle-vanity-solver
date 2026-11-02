# Changelog

All notable changes to **Bitcoin Puzzle & Vanity Çözücü** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-11-02

### Added
- **Native CUDA Acceleration**: Support for modern NVIDIA GPUs (NVIDIA Ada Lovelace `sm_89`, RTX 40-series and Ampere), achieving up to 1.65+ GH/s raw point generation.
- **Bilingual GUI**: Full English and Turkish interface support with on-the-fly switching and persistent language selection.
- **Bilingual CLI**: POSIX-standard CLI defaulting to English with optional `--lang tr` flag for Turkish output.
- **Multi-Address Batch Search**: Load `.txt` files containing up to 4,096 addresses on GPU (or unlimited on CPU) to scan keyspaces simultaneously with zero performance drop.
- **Real-Time Crypto Intelligence Dashboard**:
  - Live address balance checking via `blockchain.info` API.
  - Transaction count (`n_tx`) detection to identify historically active/empty addresses.
  - Alert system for funded vanity prefix discoveries.
  - In-memory rate limiting and caching to prevent API throttling.
- **Configurable GPU Architecture**: Expose `grid_size_x` and `grid_size_y` block/thread tuning via CLI flags (`--gpu-grid-x`, `--gpu-grid-y`) and one-click GUI optimization button.
- **Extensive Test Suite**: 221 unit and regression tests covering 256-bit arithmetic, SECP256K1 curves, Bloom filters, chunk generation, and GUI controllers.
- **CI/CD Pipeline**: GitHub Actions workflow for automated Windows MSVC building and test execution.

### Fixed
- **GPU Buffer Overflow**: Expanded GPU match buffer (`max_found`) from 64K to 1,048,576 items to eliminate item loss when searching dense multi-prefix target sets.
- **256-Bit Hex Subtraction & Carry**: Resolved arithmetic overflow in `distanceTo` when computing large 64-bit+ keyspace spans.
- **Turkish Encoding & Fonts**: Implemented Windows TrueType Unicode font loading (`segoeui.ttf`, `tahoma.ttf`) with Turkish glyph ranges to prevent mojibake.
- **Safe Fallback**: Automatic graceful fallback to CPU mode when target address counts exceed GPU hardware capacity.

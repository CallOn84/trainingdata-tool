# PR Title
Upgrade to C++20, update lc0 integration, and add one-game-per-file output

# PR Description

## Summary
This PR modernizes the trainingdata-tool by upgrading to C++20, updating the lc0 integration to work with the latest lc0 codebase, and improving the output format to write one game per chunk file.

## Changes

### Build System Updates
- Upgraded C++ standard from C++17 to C++20
- Set explicit Release build type
- Simplified include directories
- Added `lc0/src/utils/string.cc` to fix linker error for `StrSplit` function

### lc0 Integration Updates
- Updated lc0 source file paths to match latest lc0 structure:
  - Removed `lc0/src/chess/bitboard.cc` (no longer needed)
  - Changed `lc0/src/neural/writer.cc` to `lc0/src/trainingdata/writer.cc`
- Updated `.gitmodules` for lc0 submodule
- Added `absl/` library dependency

### Training Data Format Upgrade
- Upgraded from V4 to V6 training data format
- Replaced `V4TrainingDataHashUtil.h` with `V6TrainingDataHashUtil.h`
- Updated related source files for V6 compatibility

### Output Format Improvement
- Modified `TrainingDataWriter` to write one game per chunk file
- Each game's training positions are now isolated in their own `.gz` file
- Removed batching logic that previously combined multiple games into one file

## Testing
- Successfully built on Linux with GCC
- Verified output: Converting 5 games produces 5 separate files with varying sizes reflecting different game lengths

---

*Made with Gemini and Claude*

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <optional>

struct PuzzleEntry {
    int id = 0;
    std::string start_hex;
    std::string end_hex;
    std::string address;
    double reward = 0.0;
    int bits = 0;
};

class PuzzleDatabase {
public:
    // Load puzzles from a JSON file. If path is empty, searches default locations:
    // "puzzles.json", "configs/puzzles.json", "../puzzles.json", etc.
    // Throws std::runtime_error if an explicit path is provided but cannot be loaded/parsed.
    static std::vector<PuzzleEntry> load(const std::filesystem::path& path = "");

    // Parse puzzle entries from a JSON string directly.
    static std::vector<PuzzleEntry> parseJson(const std::string& json_str);

    // Find a puzzle entry by ID. Returns pointer or nullptr if not found.
    static const PuzzleEntry* findById(const std::vector<PuzzleEntry>& puzzles, int id);
};


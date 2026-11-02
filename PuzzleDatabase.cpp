#include "PuzzleDatabase.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace {

std::string stripHexPrefix(std::string s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        return s.substr(2);
    }
    return s;
}

} // namespace

std::vector<PuzzleEntry> PuzzleDatabase::parseJson(const std::string& json_str) {
    std::vector<PuzzleEntry> result;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json_str);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to parse puzzles JSON: ") + e.what());
    }

    nlohmann::json puzzles_array;
    if (root.is_array()) {
        puzzles_array = root;
    } else if (root.is_object() && root.contains("puzzles") && root["puzzles"].is_array()) {
        puzzles_array = root["puzzles"];
    } else {
        throw std::runtime_error("Invalid puzzles JSON schema: missing 'puzzles' array");
    }

    for (const auto& item : puzzles_array) {
        if (!item.is_object()) continue;
        PuzzleEntry entry;
        if (item.contains("id") && item["id"].is_number_integer()) {
            entry.id = item["id"].get<int>();
        }
        if (item.contains("start") && item["start"].is_string()) {
            entry.start_hex = stripHexPrefix(item["start"].get<std::string>());
        }
        if (item.contains("end") && item["end"].is_string()) {
            entry.end_hex = stripHexPrefix(item["end"].get<std::string>());
        }
        if (item.contains("address") && item["address"].is_string()) {
            entry.address = item["address"].get<std::string>();
        }
        if (item.contains("reward") && item["reward"].is_number()) {
            entry.reward = item["reward"].get<double>();
        }
        if (item.contains("bits") && item["bits"].is_number_integer()) {
            entry.bits = item["bits"].get<int>();
        }
        result.push_back(entry);
    }

    return result;
}

std::vector<PuzzleEntry> PuzzleDatabase::load(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> candidates;
    if (!path.empty()) {
        candidates.push_back(path);
    } else {
        candidates.push_back("puzzles.json");
        candidates.push_back("configs/puzzles.json");
        candidates.push_back("../puzzles.json");
        candidates.push_back("../configs/puzzles.json");
    }

    std::filesystem::path found_path;
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
            found_path = p;
            break;
        }
    }

    if (found_path.empty()) {
        if (!path.empty()) {
            throw std::runtime_error("Puzzles file not found: " + path.string());
        }
        return {};
    }

    std::ifstream file(found_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open puzzles file: " + found_path.string());
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parseJson(buffer.str());
}

const PuzzleEntry* PuzzleDatabase::findById(const std::vector<PuzzleEntry>& puzzles, int id) {
    for (const auto& entry : puzzles) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}


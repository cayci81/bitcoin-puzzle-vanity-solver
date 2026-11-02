#pragma once

#include <string>
#include <unordered_map>
#include <utility>

namespace puzzle {

enum class AppLanguage {
  TR = 0,
  EN = 1
};

class Localization {
 public:
  static const char* Get(const std::string& key, AppLanguage lang = AppLanguage::TR);
  static const std::unordered_map<std::string, std::pair<std::string, std::string>>& GetAllStrings();
};

inline const char* L(const std::string& key, AppLanguage lang = AppLanguage::TR) {
  return Localization::Get(key, lang);
}

}  // namespace puzzle

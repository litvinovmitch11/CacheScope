#pragma once

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace cache_simulator {
inline bool json_content_type(std::string type) {
  type = type.substr(0, type.find(';'));
  const auto begin = type.find_first_not_of(" \t");
  type = begin == std::string::npos
             ? ""
             : type.substr(begin, type.find_last_not_of(" \t") - begin + 1);
  std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return type == "application/json";
}
inline nlohmann::json parse_api_json(const std::string &body) {
  using Json = nlohmann::json;
  std::vector<std::set<std::string>> objects;
  return Json::parse(body, [&](int, Json::parse_event_t event, Json &value) {
    if (event == Json::parse_event_t::object_start)
      objects.emplace_back();
    else if (event == Json::parse_event_t::object_end)
      objects.pop_back();
    else if (event == Json::parse_event_t::key &&
             !objects.back().insert(value.get<std::string>()).second)
      throw std::invalid_argument("duplicate JSON object field");
    return true;
  });
}
} // namespace cache_simulator

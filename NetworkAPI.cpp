#include "NetworkAPI.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

std::map<std::string, uint64_t> NetworkAPI::ParseBalanceJson(const std::string& json_str) {
    std::map<std::string, uint64_t> balances;
    if (json_str.empty()) return balances;

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json_str);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to parse balance JSON: ") + e.what());
    }

    if (!root.is_object()) {
        throw std::runtime_error("Balance response is not a JSON object");
    }

    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string& addr = it.key();
        const auto& val = it.value();
        if (val.is_object() && val.contains("final_balance") && val["final_balance"].is_number_integer()) {
            balances[addr] = val["final_balance"].get<uint64_t>();
        }
    }

    return balances;
}

std::map<std::string, uint64_t> NetworkAPI::QueryBalances(
    const std::vector<std::string>& addresses,
    int timeout_seconds)
{
    if (addresses.empty()) return {};

    std::ostringstream oss;
    for (size_t i = 0; i < addresses.size(); ++i) {
        if (i > 0) oss << "|";
        oss << addresses[i];
    }

    std::string url = "https://blockchain.info/balance?active=" + oss.str();
    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Timeout{timeout_seconds * 1000},
        cpr::Header{{"User-Agent", "Puzzle71Solver/1.0"}}
    );

    if (r.error) {
        throw std::runtime_error("Network error querying balance: " + r.error.message);
    }
    if (r.status_code != 200) {
        throw std::runtime_error("HTTP error " + std::to_string(r.status_code) + " querying balance");
    }

    return ParseBalanceJson(r.text);
}


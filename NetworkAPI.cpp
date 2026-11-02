#include "NetworkAPI.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <thread>

std::unordered_map<std::string, AddressInfo> NetworkAPI::cache_;
std::mutex NetworkAPI::cache_mu_;

std::chrono::steady_clock::time_point NetworkAPI::last_request_time_{};
std::mutex NetworkAPI::rate_limit_mu_;

std::function<std::string(const std::string&, int)> NetworkAPI::http_fetcher_ = nullptr;

std::map<std::string, AddressInfo> NetworkAPI::ParseBalanceJson(const std::string& json_str) {
    std::map<std::string, AddressInfo> balances;
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
        if (val.is_object()) {
            AddressInfo info;
            if (val.contains("final_balance") && val["final_balance"].is_number_integer()) {
                info.balance_satoshis = val["final_balance"].get<uint64_t>();
            }
            if (val.contains("n_tx") && val["n_tx"].is_number_integer()) {
                info.tx_count = val["n_tx"].get<uint32_t>();
            }
            balances[addr] = info;
        }
    }

    return balances;
}

std::map<std::string, AddressInfo> NetworkAPI::QueryBalances(
    const std::vector<std::string>& addresses,
    int timeout_seconds)
{
    if (addresses.empty()) return {};

    std::vector<std::string> missing;
    {
        std::lock_guard<std::mutex> lock(cache_mu_);
        for (const auto& addr : addresses) {
            if (cache_.find(addr) == cache_.end()) {
                missing.push_back(addr);
            }
        }
    }

    if (!missing.empty()) {
        std::ostringstream oss;
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i > 0) oss << "|";
            oss << missing[i];
        }

        std::string url = "https://blockchain.info/balance?active=" + oss.str();
        std::string response_text;

        // Rate limiting: guarantee at least 1 second between network API requests
        {
            std::lock_guard<std::mutex> lock(rate_limit_mu_);
            if (last_request_time_.time_since_epoch().count() > 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_request_time_);
                if (elapsed < std::chrono::milliseconds(1000)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000) - elapsed);
                }
            }
            last_request_time_ = std::chrono::steady_clock::now();
        }

        if (http_fetcher_) {
            response_text = http_fetcher_(url, timeout_seconds);
        } else {
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
            response_text = r.text;
        }

        auto parsed = ParseBalanceJson(response_text);
        {
            std::lock_guard<std::mutex> lock(cache_mu_);
            for (const auto& [addr, info] : parsed) {
                cache_[addr] = info;
            }
            // For any requested address not present in response, cache a zero entry
            for (const auto& addr : missing) {
                if (cache_.find(addr) == cache_.end()) {
                    cache_[addr] = AddressInfo{0, 0};
                }
            }
        }
    }

    std::map<std::string, AddressInfo> results;
    {
        std::lock_guard<std::mutex> lock(cache_mu_);
        for (const auto& addr : addresses) {
            auto it = cache_.find(addr);
            if (it != cache_.end()) {
                results[addr] = it->second;
            }
        }
    }

    return results;
}

void NetworkAPI::ClearCache() {
    std::lock_guard<std::mutex> lock(cache_mu_);
    cache_.clear();
}

size_t NetworkAPI::CacheSize() {
    std::lock_guard<std::mutex> lock(cache_mu_);
    return cache_.size();
}

bool NetworkAPI::IsCached(const std::string& address) {
    std::lock_guard<std::mutex> lock(cache_mu_);
    return cache_.find(address) != cache_.end();
}

void NetworkAPI::SetHttpFetcherForTesting(std::function<std::string(const std::string&, int)> fetcher) {
    http_fetcher_ = std::move(fetcher);
}

void NetworkAPI::ResetHttpFetcher() {
    http_fetcher_ = nullptr;
}

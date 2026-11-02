#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <mutex>
#include <chrono>
#include <functional>

struct AddressInfo {
    uint64_t balance_satoshis = 0;
    uint32_t tx_count = 0;
};

class NetworkAPI {
public:
    // Query balance and tx_count for one or more Bitcoin addresses using blockchain.info API.
    // Checks the in-memory cache first, queries only missing addresses, respects rate limits,
    // and returns results for all requested addresses.
    // Throws std::runtime_error on network or HTTP error.
    static std::map<std::string, AddressInfo> QueryBalances(
        const std::vector<std::string>& addresses,
        int timeout_seconds = 10);

    // Parse the JSON returned by blockchain.info /balance API.
    // Address -> AddressInfo (balance in satoshis, transaction count).
    static std::map<std::string, AddressInfo> ParseBalanceJson(const std::string& json_str);

    // Cache management
    static void ClearCache();
    static size_t CacheSize();
    static bool IsCached(const std::string& address);

    // Hook for dependency injection / mock testing without live network calls
    static void SetHttpFetcherForTesting(std::function<std::string(const std::string& url, int timeout_seconds)> fetcher);
    static void ResetHttpFetcher();

private:
    static std::unordered_map<std::string, AddressInfo> cache_;
    static std::mutex cache_mu_;

    static std::chrono::steady_clock::time_point last_request_time_;
    static std::mutex rate_limit_mu_;

    static std::function<std::string(const std::string& url, int timeout_seconds)> http_fetcher_;
};

#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

class NetworkAPI {
public:
    // Query balance (in satoshis) for one or more Bitcoin addresses using blockchain.info API.
    // Throws std::runtime_error on network or HTTP error.
    static std::map<std::string, uint64_t> QueryBalances(
        const std::vector<std::string>& addresses,
        int timeout_seconds = 10);

    // Parse the JSON returned by blockchain.info /balance API.
    // Address -> balance in satoshis.
    static std::map<std::string, uint64_t> ParseBalanceJson(const std::string& json_str);
};

